# README

## Goal

This project aims to provide CLI commands to access basic hardware functionality on STM32 Nucleo boards.

Provided functionnality would include : configuring and using the GPIOS, ADCs, DACs, PWM.

The project includes a python interface that can automate the CLI commands on a PC connected
to the board.

The project will include a web app to configure the GPIOS
and provide a UI for accessing the functionality provided by the CLI.

It is a learning project, more features will be added as I develop them.

## Hardware

Currently the project is developped on a Nucleo-G474RE board and only supports that board. Other
Nucleo boards might be added later.

You will need a serial adapter to access the board, for instance a USB-serial adapter such as a CP2102.

The current project uses two UARTS (LPUART for printf() and USART 1 for the CLI) set by default at 115200 8N1.

## Build environment

The project is based on a CMake project generated from CubeMX. It will compile and run on VSCode on Linux
after installing the _STM32CubeIDE for Visual Studio Code_ extension.

## Yocto
