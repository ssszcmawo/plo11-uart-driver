
# PLO11 UART Driver

Custom UART driver for the Linux kernel implementing a platform-based serial device with Device Tree support.



## Overview

This driver implements a UART controller named **PLO11**, inspired by memory-mapped UART designs. It integrates with the Linux serial core (`serial_core`) and provides a standard TTY interface.

The driver supports:

* RX/TX via interrupt handling
* Configurable baud rate
* Integration with Device Tree
* Multiple UART instances (2)



## Device Tree Example

```dts
/ {
    aliases {
        serial0 = &plo11_0;
        serial1 = &plo11_1;
    };

    plo11_0: serial@10000000 {
        compatible = "plo11";
        reg = <0x10000000 0x1000>;
        interrupts = <5>;
        status = "okay";
    };

    plo11_1: serial@10001000 {
        compatible = "plo11";
        reg = <0x10001000 0x1000>;
        interrupts = <6>;
        status = "okay";
    };
};
```


