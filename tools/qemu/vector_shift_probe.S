    .text
    .option arch, +v
    .global ProbeShift
    .type ProbeShift, @function
ProbeShift:
    vsetivli zero,16,e8,m1,tu,mu
    vle8.v v8,(a0)
    li a1,7
    vsra.vx v10,v8,a1
    vse8.v v10,(a2)
    ret
    .size ProbeShift, .-ProbeShift
    .global ProbeImmediate
    .type ProbeImmediate, @function
ProbeImmediate:
    vsetivli zero,16,e8,m1,tu,mu
    vle8.v v8,(a0)
    vsra.vi v10,v8,7
    vse8.v v10,(a2)
    ret
    .size ProbeImmediate, .-ProbeImmediate
    .section .note.GNU-stack,"",@progbits
