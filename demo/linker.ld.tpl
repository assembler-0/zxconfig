/* ${BenchProject}: ${arch.BenchTarget}, host ${BenchHostOs}/${BenchHostArch} */
ENTRY(_start)
SECTIONS {
    . = ${arch.BenchBaseAddress};
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
/* dependency chain terminal: ${dep.Chain0256} */
/* $$ escapes a literal dollar: $$BenchProject */
