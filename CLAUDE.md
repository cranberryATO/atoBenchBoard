# CLAUDE.md

## Project

This is a Nucleo-G474RE STM32 projetc.
The project is generated with CubeMX, for CMake, to be used on VSCode.

## Modification of code

Don't modify the code unless asked to.

## Language

All code and comments in English only.

## Error handling

We don't try to recover from major errors such as OOM or task could not create.
Such errors should trigger a fatalError.
This simplifies the code : functions that are necessary for the system to function normally
are not allowed to return errors, they must fail with fatalError.
