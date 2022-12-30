#!/bin/bash -e

openocd -s ../openocd -f openocd.cfg -c 'stm32f0xx_flash relayclock.bin'
