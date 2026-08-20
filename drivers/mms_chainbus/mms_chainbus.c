/* SPDX-License-Identifier: Apache-2.0 */

#define DT_DRV_COMPAT konar_mms_chainbus

#include <drivers/mms_chainbus.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(chainbus, CONFIG_MMS_CHAINBUS_LOG_LEVEL);

#define MAX_CHAINBUS_SLOTS 8

/* EEPROM Hardware Definitions */
#define EEPROM_I2C_ADDR 0x50
#define EEPROM_PAGE0_ID_PTR_ADDR 0x0002
#define EEPROM_NAME_OFFSET 16
#define EEPROM_NAME_LEN 24

/* Devicetree Alias for Baseboard I2C Controller */
#define I2C_BUS_NODE DT_ALIAS(chainbus_i2c)

struct slot_entry {
    bool reserved;
    bool is_static;
    bool claimed_by_driver;
    char name_buf[EEPROM_NAME_LEN + 1]; /* Holds dynamic name + NUL byte */
    const char* compatible;
    uint32_t sw_version;
    uint32_t hw_revision;
};

struct chainbus_config {
    struct gpio_dt_spec cs_gpios[MAX_CHAINBUS_SLOTS];
    uint8_t num_gpios;
};

struct chainbus_data {
    struct slot_entry slots[MAX_CHAINBUS_SLOTS];
};

static const struct chainbus_config chainbus_cfg = {
    .cs_gpios = {DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(0), cs_gpios, GPIO_DT_SPEC_GET_BY_IDX, (, ))},
    .num_gpios = DT_PROP_LEN(DT_DRV_INST(0), cs_gpios),
};

static struct chainbus_data chainbus_dat;

/**
 * @brief Reads the HAT identification block from EEPROM over I2C.
 */
static const char* probe_hardware_slot(uint8_t slot, const struct gpio_dt_spec* cs_gpio,
                                       struct slot_entry* slot_data) {
#if !DT_NODE_EXISTS(I2C_BUS_NODE)
    LOG_WRN_ONCE("DT Alias 'chainbus_i2c' not defined in Devicetree!");
    return NULL;
#else
    const struct device* i2c_dev = DEVICE_DT_GET(I2C_BUS_NODE);

    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C bus 'chainbus_i2c' is not ready");
        return NULL;
    }

    /* 1. Select the HAT (drive CS line active) */
    gpio_pin_set_dt(cs_gpio, 1);

    /* 2. Read 'id_data_pointer' at address 0x0002 from Page 0 */
    uint8_t ptr_reg_be[2] = {0x00, EEPROM_PAGE0_ID_PTR_ADDR};
    uint8_t ptr_buf[2] = {0};

    int ret = i2c_write_read(i2c_dev, EEPROM_I2C_ADDR, ptr_reg_be, 2, ptr_buf, 2);
    if (ret < 0) {
        LOG_DBG("Slot %d: No EEPROM responding at I2C address 0x%02X", slot, EEPROM_I2C_ADDR);
        gpio_pin_set_dt(cs_gpio, 0);
        return NULL;
    }

    /* ID Data Pointer is Little Endian */
    uint16_t id_ptr = sys_get_le16(ptr_buf);

    /* Check for blank (0xFFFF) or unprogrammed (0x0000) pointer */
    if (id_ptr == 0x0000 || id_ptr == 0xFFFF) {
        LOG_DBG("Slot %d: Blank or unprogrammed EEPROM (ID ptr: 0x%04X)", slot, id_ptr);
        gpio_pin_set_dt(cs_gpio, 0);
        return NULL;
    }

    /* 3. Read ID block starting at (id_ptr + 16):
     *    - Name: 24 bytes
     *    - Software Version: 4 bytes
     *    - Hardware Revision: 4 bytes
     *    Total = 32 bytes in a single transaction
     */
    uint16_t id_block_addr = id_ptr + EEPROM_NAME_OFFSET;
    uint8_t id_reg_be[2] = {(id_block_addr >> 8) & 0xFF, id_block_addr & 0xFF};
    uint8_t id_data[32] = {0};

    ret = i2c_write_read(i2c_dev, EEPROM_I2C_ADDR, id_reg_be, 2, id_data, sizeof(id_data));

    /* Deselect HAT immediately after read finishes */
    gpio_pin_set_dt(cs_gpio, 0);

    if (ret < 0) {
        LOG_WRN("Slot %d: Failed reading ID block at 0x%04X", slot, id_block_addr);
        return NULL;
    }

    /* 4. Extract fields */
    /* Name field: 24 bytes, zero-padded, NOT NUL-terminated in EEPROM */
    memcpy(slot_data->name_buf, &id_data[0], EEPROM_NAME_LEN);
    slot_data->name_buf[EEPROM_NAME_LEN] = '\0'; /* Null-terminate for C string use */

    /* Software Version and Hardware Revision are Little Endian uint32_t */
    slot_data->sw_version = sys_get_le32(&id_data[24]);
    slot_data->hw_revision = sys_get_le32(&id_data[28]);

    LOG_INF("Slot %d: Discovered HAT '%s' (SW Ver: %u, HW Rev: %u)", slot, slot_data->name_buf,
            slot_data->sw_version, slot_data->hw_revision);

    return slot_data->name_buf;
#endif
}

/* Helper Macro for Pass 1: Static DT reservations */
#define REGISTER_STATIC_CHILD(node_id)                                                             \
    IF_ENABLED(DT_NODE_HAS_PROP(node_id, position), (do {                                          \
                   int pos = DT_PROP(node_id, position);                                           \
                   if (pos < MAX_CHAINBUS_SLOTS) {                                                 \
                       chainbus_dat.slots[pos].reserved = true;                                    \
                       chainbus_dat.slots[pos].is_static = true;                                   \
                       chainbus_dat.slots[pos].compatible =                                        \
                           DT_PROP_BY_IDX(node_id, compatible, 0);                                 \
                       LOG_INF("Pass 1: Static slot %d locked for %s", pos,                        \
                               chainbus_dat.slots[pos].compatible);                                \
                   }                                                                               \
               } while (0);))

static int chainbus_init(const struct device* dev) {
    const struct chainbus_config* config = dev->config;
    struct chainbus_data* data = dev->data;
    int ret;

    /* Configure CS GPIOs as inactive outputs */
    for (uint8_t i = 0; i < config->num_gpios; i++) {
        if (!gpio_is_ready_dt(&config->cs_gpios[i])) {
            LOG_ERR("GPIO line for slot %d not ready", i);
            return -ENODEV;
        }
        ret = gpio_pin_configure_dt(&config->cs_gpios[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            return ret;
        }
    }

    /* PASS 1: Lock down statically assigned DT slots */
    DT_FOREACH_CHILD(DT_DRV_INST(0), REGISTER_STATIC_CHILD);

    /* PASS 2: Auto-discover remaining unreserved physical slots */
    for (uint8_t slot = 0; slot < config->num_gpios; slot++) {
        if (data->slots[slot].reserved) {
            LOG_DBG("Pass 2: Skipping slot %d (static lock)", slot);
            continue;
        }

        const char* discovered =
            probe_hardware_slot(slot, &config->cs_gpios[slot], &data->slots[slot]);
        if (discovered != NULL) {
            data->slots[slot].reserved = true;
            data->slots[slot].is_static = false;
            data->slots[slot].compatible = discovered;
        }
    }

    return 0;
}

int chainbus_claim_slot(const char* compatible, int position) {
    struct chainbus_data* data = &chainbus_dat;

    for (int i = 0; i < chainbus_cfg.num_gpios; i++) {
        if (!data->slots[i].reserved || data->slots[i].claimed_by_driver) {
            continue;
        }

        if (strcmp(data->slots[i].compatible, compatible) == 0) {
            if (position >= 0 && data->slots[i].is_static && i != position) {
                continue;
            }
            data->slots[i].claimed_by_driver = true;
            LOG_INF("Driver claimed slot %d for %s", i, compatible);
            return i;
        }
    }

    return -ENODEV;
}

int chainbus_assert_cs(int slot, bool active) {
    if (slot < 0 || slot >= chainbus_cfg.num_gpios) {
        return -EINVAL;
    }
    return gpio_pin_set_dt(&chainbus_cfg.cs_gpios[slot], active ? 1 : 0);
}

const struct gpio_dt_spec* chainbus_get_cs_spec(int slot) {
    if (slot < 0 || slot >= chainbus_cfg.num_gpios) {
        return NULL;
    }
    return &chainbus_cfg.cs_gpios[slot];
}

DEVICE_DT_INST_DEFINE(0, chainbus_init, NULL, &chainbus_dat, &chainbus_cfg, POST_KERNEL,
                      CONFIG_CHAINBUS_INIT_PRIORITY, NULL);
