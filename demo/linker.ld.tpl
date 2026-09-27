/* generated demo linker script */
ENTRY(_start)
SECTIONS {
    . = ${BASE_ADDR};
    .text : { *(.text*) }
}
/* $$ is a literal dollar: $$BASE_ADDR is not interpolated */
