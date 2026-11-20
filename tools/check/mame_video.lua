-- Original MAME 0.264 video state, captured before register/list changes and
-- at the motion-object device's 64-line partial-update boundaries.
-- No game RAM is changed. CPU execution and rendering remain MAME's own.
return function(m, wanted, out, final_frame)
 local s=m.screens[':screen']
 local shares=m.memory.shares
 local xs,ys=shares[':xscroll'],shares[':yscroll']
 local bank=emu.item(m.devices[':slapstic'].items['0/m_current_bank'])
 local file=assert(io.open(out..'/mame-video.capture','wb'))
 file:write('APBVID1\0')
 local bands,pictures=0,0
 local nextline={}
 local effective_y,pending_y,last_frame=0,nil,-1
 local function at() return math.floor(m.time:as_double()*10000000+0.5) end
 local function frame() return math.floor((at()-12800)/166400) end
 local function vpos() return math.floor(((at()+153600)%166400)/400) end
 local function sync_y()
  local f=frame()
  if f~=last_frame then
   if pending_y~=nil then effective_y=pending_y;pending_y=nil end
   last_frame=f
  end
 end
 local function memory(share)
  local words={}
  for offset=0,share.size-1,2 do words[#words+1]=string.pack('<I2',share:read_u16(offset)) end
  return table.concat(words)
 end
 local function capture(f,last)
  sync_y()
  if not wanted[f] or last>=384 then return end
  local first=nextline[f] or 0
  if last<first then return end
  file:write('B',string.pack('<I4I2I2I2I2i4',f,first,last,xs:read_u16(0),ys:read_u16(0),effective_y))
  file:write(memory(shares[':alpha']),memory(shares[':mob']),memory(shares[':playfieldt']),memory(shares[':playfieldb']))
  bands=bands+1
  nextline[f]=last+1
 end
 local space=m.devices[':maincpu'].spaces['program']
 apb_video_taps={}
 apb_video_taps[1]=space:install_write_tap(0x1700,0x17ff,'apb-scroll-snapshot',function(a,d,mask)
  sync_y()
  local vertical=(a&0x80)~=0
  local old=(vertical and ys or xs):read_u16(0)
  local value=(old&(~mask))|(d&mask)
  if value~=old then capture(frame(),vpos()) end
  if vertical then
   if (value&0x10)~=0 then pending_y=value>>6 else effective_y=(value>>6)-vpos() end
  end
 end)
 apb_video_taps[2]=space:install_write_tap(0x3806,0x3807,'apb-list-snapshot',function(a,d,mask)
  if bank:read(0)==0 then capture(frame(),vpos()) end
 end)
 return {
  frame_done=function(n)
   if wanted[n] then
    assert(frame()==n,'Unexpected MAME raster origin')
    capture(n,383)
    assert(nextline[n]==384,'Incomplete video frame')
    file:write('P',string.pack('<I4',n),memory(shares[':palette']))
    pictures=pictures+1
   end
  end,
  finish=function()
   file:write('E',string.pack('<I4I4',bands,pictures));file:close()
  end,
  run=function()
   while true do
    for line=64,320,64 do
     emu.wait(s:time_until_pos(line))
     if frame()>final_frame then return end
     capture(frame(),line-1)
    end
   end
  end
 }
end
