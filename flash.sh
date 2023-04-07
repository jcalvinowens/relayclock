#!/bin/bash -e

#openocd -s ../openocd/tcl -f openocd-rpi.cfg -c 'stm32f0xx_flash relayclock.bin'
openocd -f openocd.cfg -c 'stm32f0xx_flash relayclock.bin'
