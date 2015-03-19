#EvvGC-PLUS
**EvvGC-PLUS** is open source software and firmware hobby project for EvvGC brushless gimbal controller v1.x.

**FEATURES of EvvGC-PLUS:**
* EvvGC-PLUS firmware is based on [ChibiOS/RT](http://chibios.org "ChibiOS Homepage") real time operating system (**RTOS**) for easy multitasking.
* ChibiOS/RT hardware abstraction layer (**HAL**) is used for efficient, usually DMA based control of the STM32 peripherals.
* Quaternion based attitude estimation loop runs at 500 Hz.
* Motor driving efficiency is increased by ~14% using third harmonic injection technique.
* Any orientation of the sensor is possible.
* Communications with two sensors is enabled.
* PID controller is based on motor speed.
* Feed forward is used to improve disturbance rejection.
* EvvGC-PLUS configurator is written using [Qt framework](http://qt-project.org "Qt Homepage").
* [QCustomPlot](http://www.qcustomplot.com "QCustomPlot Homepage") is used for easy plotting and data visualization.
* Dedicated thread is used for serial communications.

**DO NOT DOWNLOAD, BUILD, FLASH OR INSTALL IT IF YOU DON'T KNOW WHAT YOU ARE DOING!!!**
