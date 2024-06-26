connect -url tcp:127.0.0.1:3121
source Z:/vm/openSSD-fw-ori/cosmos8c8w/ps7_init.tcl
targets -set -nocase -filter {name =~"APU*" && jtag_cable_name =~ "Digilent JTAG-SMT2 210251A015B5"} -index 0
rst -system
after 3000
targets -set -filter {jtag_cable_name =~ "Digilent JTAG-SMT2 210251A015B5" && level==0} -index 1
fpga -file Z:/vm/openSSD-fw-ori/cosmos8c8w/sys_top_wrapper.bit
targets -set -nocase -filter {name =~"APU*" && jtag_cable_name =~ "Digilent JTAG-SMT2 210251A015B5"} -index 0
loadhw -hw Z:/vm/openSSD-fw-ori/cosmos8c8w/system.hdf -mem-ranges [list {0x40000000 0xbfffffff}]
configparams force-mem-access 1
targets -set -nocase -filter {name =~"APU*" && jtag_cable_name =~ "Digilent JTAG-SMT2 210251A015B5"} -index 0
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "ARM*#0" && jtag_cable_name =~ "Digilent JTAG-SMT2 210251A015B5"} -index 0
dow Z:/vm/openSSD-fw-ori/openssd/Debug/openssd.elf
configparams force-mem-access 0
bpadd -addr &main
