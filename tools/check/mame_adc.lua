-- Standalone ADC0809 probe for original MAME 0.264, run at autoboot delay 0.
-- This diagnostic writes ADC selection/inputs and exits immediately; do not
-- load it into the full-game baseline capture.
local m=manager.machine
local main=m.devices[':maincpu'].spaces['program']
local pedal=m.ioport.ports[':ADC1'].fields['P1 Pedal 1']
local function report(label,expected)
 local result=main:read_u16(0x1400)
 print('ADC',label,math.floor(m.time:as_double()*10000000+0.5),result)
 assert(result==expected,'ADC probe mismatch at '..label)
end
pedal:set_value(17)
report('initial',0xffff)
main:write_u8(0x1482,0)
emu.wait(0.0000031)
report('31',0xffff)
emu.wait(0.0000002)
report('33',0xff11)
pedal:set_value(23)
emu.wait(0.0001022)
report('1055',0xff11)
emu.wait(0.0000002)
report('1057',0xff17)
main:write_u8(0x148a,0)
emu.wait(0.0000033)
report('unconnected',0xffff)
print('ADC probe passed')
m:exit()
