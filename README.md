# MMS Chainbus Module

## Overview

The MMS Chainbus module provides a Zephyr-compatible driver for the **Konar MMS Chainbus** hardware controller. This module manages up to 8 physical HAT (Hardware Attached on Top) slots via Chip Select (CS) GPIO lines, enabling communication with MMS HAT modules that expose I2C and/or SPI interfaces.

The Chainbus controller performs **two-pass slot initialization**:

1. **Pass 1 (Static)**: Locks down slots explicitly reserved in Devicetree via `position` property
2. **Pass 2 (Dynamic)**: Auto-discovers HATs in remaining slots by reading identification EEPROMs over I2C

Child HAT containers (via the `mms_hat` module) claim slots by compatible string, and virtual I2C/SPI bridges route transactions to the physical buses while managing CS lines automatically.

## Features

- **8-slot HAT manager** with individual CS GPIO control per slot
- **Devicetree-driven static slot reservation** via `position` property
- **Runtime auto-discovery** via EEPROM identification (name, SW version, HW revision)
- **Compatible with [mms_hat](https://github.com/maxksiazka/mms_hat_driver) module** for HAT container and bridge drivers
- **Zephyr logging integration** with configurable log level

## Hardware Requirements

- **MMS3 baseboard** (e.g., `konar,stm32f405-chainbus`)
- **MMS HAT modules** with identification EEPROM (24Cxx compatible) at I2C address `0x50`
- **I2C bus** for EEPROM probing (aliased as `chainbus-i2c` in board DT)
- **8 GPIO lines** for slot CS signals (active-low)

## Module Structure

```
modules/mms_chainbus/
├── dts/bindings/mms/konar,mms-chainbus.yaml   # Devicetree binding
├── include/drivers/mms_chainbus.h              # Public API header
├── drivers/mms_chainbus/
│   ├── mms_chainbus.c                          # Core implementation
│   ├── Kconfig                                 # Kconfig options
│   └── CMakeLists.txt
├── zephyr/module.yml                           # Zephyr module metadata
├── Kconfig                                     # Top-level Kconfig entry
└── CMakeLists.txt
```

## Dependencies

- **Zephyr subsystems**: GPIO, I2C, Logging

```conf
# prj.conf
CONFIG_MMS_CHAINBUS=y
CONFIG_I2C=y
CONFIG_SPI=y
CONFIG_LOG=y
```

## Devicetree Configuration

### Board Level (Baseboard)

Define the Chainbus controller with 8 CS GPIOs and child HAT nodes:

```dts
/* Board DTS (e.g., boards/konar/mms3/konar_mms3.dts) */
aliases {
    chainbus-i2c = &i2c2;   /* Physical I2C for EEPROM probing */
    chainbus-spi = &spi3;   /* Physical SPI for HAT SPI bridges */
};

/* Optional: define CS GPIOs in a gpio-keys node for reference */
chainbus-hat-select {
    compatible = "gpio-keys";
    hat_sel_1: pos_1 { gpios = <&gpioc 9 GPIO_ACTIVE_LOW>; };
    hat_sel_2: pos_2 { gpios = <&gpiob 5 GPIO_ACTIVE_LOW>; };
    /* ... up to 8 slots ... */
};
```

### Application Level (Overlay)

Instantiate the Chainbus controller and declare HATs:

```dts
/* App overlay (e.g., apps/1-blink/boards/konar_mms3.overlay) */
chainbus_i2c: &i2c2 {};
chainbus_spi: &spi3 {};

/ {
    aliases {
        chainbus-led0 = &hat0_led0;
        /* ... chainbus-led7 = &hat0_led7; */
        rtc-clock = &rtc0;
    };

    chainbus0: chainbus {
        compatible = "konar,mms-chainbus";
        status = "okay";

        /* 8 Slot Chip Select GPIOs (must match board wiring order) */
        cs-gpios = <&gpioc 9 GPIO_ACTIVE_LOW>,  /* Slot 0 */
               <&gpiob 5 GPIO_ACTIVE_LOW>,  /* Slot 1 */
               <&gpioa 7 GPIO_ACTIVE_LOW>,  /* Slot 2 */
               <&gpioa 6 GPIO_ACTIVE_LOW>,  /* Slot 3 */
               <&gpioa 5 GPIO_ACTIVE_LOW>,  /* Slot 4 */
               <&gpioc 6 GPIO_ACTIVE_LOW>,  /* Slot 5 */
               <&gpioc 7 GPIO_ACTIVE_LOW>,  /* Slot 6 */
               <&gpioc 8 GPIO_ACTIVE_LOW>;  /* Slot 7 */

        #address-cells = <1>;
        #size-cells = <0>;

        /* HAT definitions go here, either static or dynamic discovery */

};
```

**Key Devicetree Properties:**

| Property           | Type          | Required | Description                                      |
| ------------------ | ------------- | -------- | ------------------------------------------------ |
| `compatible`       | string        | Yes      | Must be `"konar,mms-chainbus"`                   |
| `cs-gpios`         | phandle-array | Yes      | 8 GPIO specifiers for slot CS lines (active-low) |
| `position` (child) | int           | No       | Static slot index (0-7). Omit for auto-discovery |

## API Reference

Header: `#include <drivers/mms_chainbus.h>`

### Slot Claiming (used by HAT container drivers)

```c
/**
 * @brief Claim a physical slot for a specific HAT compatible string.
 *
 * @param compatible DT compatible string of the HAT (e.g., "konar,mms-hat")
 * @param position Desired static position, or -1 for dynamic match
 * @return Assigned physical slot index (>= 0), or negative error code
 */
int chainbus_claim_slot(const char* compatible, int position);
```

### CS Line Control

```c
/**
 * @brief Assert or de-assert the CS GPIO for a specific physical slot.
 *
 * @param slot Physical slot index (0..7)
 * @param active True to drive line active (select), false to drive inactive (deselect)
 * @return 0 on success, negative error code on failure
 */
int chainbus_assert_cs(int slot, bool active);

/**
 * @brief Get the GPIO specifier associated with a physical slot.
 *
 * @param slot Physical slot index
 * @return Pointer to gpio_dt_spec, or NULL if slot is invalid
 */
const struct gpio_dt_spec* chainbus_get_cs_spec(int slot);
```

> **Note**: Application code typically does not _and should not_ call these directly.
> The `mms_hat` container driver calls `chainbus_claim_slot()` during init, and the virtual I2C/SPI bridges call `chainbus_assert_cs()` automatically around each transaction.

## Runtime Behavior

### Two-Pass Initialization

1. **Pass 1 (Static Reservation)**: During `chainbus_init()`, the driver iterates all child nodes. Any child with a `position` property marks that slot as `reserved` and `is_static`, storing the child's `compatible` string.

2. **Pass 2 (Auto-Discovery)**: For each non-reserved slot, the driver:
   - Asserts the slot's CS GPIO
   - Reads the HAT EEPROM at I2C address `0x50`
   - Reads the ID data pointer at EEPROM offset `0x0002`
   - Reads the identification block (24-byte name + 4-byte SW version + 4-byte HW revision)
   - Stores discovered name as `compatible` string, marks slot `reserved`

### Slot Claiming by HAT Containers

> **Note**: This is handled by the `mms_hat` module, not directly by application code. Unless you are writing a new HAT container driver, you do not need to call these functions.
> See [MMS HAT Module](https://github.com/maxksiazka/mms_hat_driver) for details.

Each HAT container driver (from `mms_hat` module) calls `chainbus_claim_slot(compatible, position)` at init:

- Matches against `slot_entry.compatible` (either static DT string or discovered EEPROM name)
- If `position >= 0` and slot is static, enforces exact slot match
- Marks slot `claimed_by_driver` to prevent double-claiming

### Virtual Bus Bridges

The `mms_hat` module provides:

- **`konar,mms-hat-i2c-bridge`**: Implements `i2c_driver_api`; wraps `i2c_transfer()` with `chainbus_assert_cs(slot, true/false)`
- **`konar,mms-hat-spi-bridge`**: Implements `spi_driver_api`; wraps `spi_transceive()` with the same CS management.

These appear as standard I2C/SPI controllers to child drivers (e.g., RTC, GPIO expander, sensors).

## Usage Example

The [MMS blink](https://github.com/maxksiazka/mms_blink) sample demonstrates a complete setup:

1. **Board**: [MMS3 STM32F405 baseboard repository](https://github.com/maxksiazka/mms_f405_zephyr_board_def) (STM32F405, I2C2 + SPI3 + 8 CS GPIOs)
2. **Overlay**: `boards/konar_mms3.overlay` (Chainbus + static HAT @ slot 0)
3. **Application**: `src/main.c` (toggles LEDs on GPIO expander via virtual I2C)

```c
/* main.c - using virtual I2C bridge transparently */
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>

/* LEDs aliased to GPIOs on the virtual I2C bridge's expander */
static const struct gpio_dt_spec chainbus_led[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(chainbus_led0), gpios),
    /* ... */
};

int main(void) {
    /* Configure LEDs - goes through virtual I2C bridge -> Chainbus CS -> PCA9555 */
    for (int i = 0; i < 8; i++) {
        gpio_pin_configure_dt(&chainbus_led[i], GPIO_OUTPUT_ACTIVE);
    }

    /* RTC accessed via virtual I2C bridge -> DS3231 */
    const struct device* rtc_dev = DEVICE_DT_GET(DT_NODELABEL(rtc0));
    struct rtc_time time = {.tm_hour = 12, .tm_min = 30, ...};
    rtc_set_time(rtc_dev, &time);

    while (1) {
        for (int i = 0; i < 8; i++) {
            gpio_pin_toggle_dt(&chainbus_led[i]);
        }
        rtc_get_time(rtc_dev, &time);
        k_msleep(1000);
    }
    return 0;
}
```

**Build:**

```bash
west build -b konar_mms3 apps/1-blink

```

## Kconfig Options

| Option                   | Type | Default           | Description                                          |
| ------------------------ | ---- | ----------------- | ---------------------------------------------------- |
| `MMS_CHAINBUS`           | bool | y (if DT present) | Enable Chainbus controller                           |
| `CHAINBUS_INIT_PRIORITY` | int  | 55                | Init priority (POST_KERNEL). Must be < HAT init (56) |
| `MMS_CHAINBUS_LOG_LEVEL` | int  | 3 (INFO)          | Log level for Chainbus module                        |

## EEPROM Identification Format

HAT modules must populate the EEPROM (24Cxx at 0x50) as follows:

| Offset     | Size         | Description                       |
| ---------- | ------------ | --------------------------------- |
| 0x0002     | 2 bytes (LE) | Pointer to ID block start address |
| `ptr + 16` | 24 bytes     | HAT name (zero-padded, ASCII)     |
| `ptr + 40` | 4 bytes (LE) | Software version (uint32)         |
| `ptr + 44` | 4 bytes (LE) | Hardware revision (uint32)        |

Example: A HAT with ID pointer `0x0100` stores its name at `0x0110`, SW version at `0x0128`, HW revision at `0x012C`.

## Troubleshooting

| Symptom                                     | Cause                                  | Fix                                                                     |
| ------------------------------------------- | -------------------------------------- | ----------------------------------------------------------------------- |
| `DT Alias 'chainbus_i2c' not defined`       | Board DTS missing alias                | Add `chainbus-i2c = &i2cX;` to aliases                                  |
| Slot discovery fails                        | EEPROM not programmed / wrong I2C addr | Verify EEPROM at 0x50, check CS GPIO wiring                             |
| `HAT failed to claim a valid Chainbus slot` | Compatible mismatch or slot taken      | Check `compatible` in HAT DT matches EEPROM name or static DT           |
| I2C/SPI transactions fail                   | CS GPIO not toggling                   | Verify `cs-gpios` order matches physical slots; check `GPIO_ACTIVE_LOW` |
| Init order errors                           | Priority conflicts                     | Chainbus (55) < HAT (56) < Bridges (60) < On-bus devices (>61)          |

## Related Modules

- **[mms_hat](https://github.com/maxksiazka/mms_hat_driver)**: HAT container driver, virtual I2C/SPI bridges
- **[apps/1-blink](https://github.com/maxksiazka/mms_blink)**: Complete working example (overlay + app)

## License

This software is licensed under **Apache 2.0**.
See [LICENSE](LICENSE) for details.
