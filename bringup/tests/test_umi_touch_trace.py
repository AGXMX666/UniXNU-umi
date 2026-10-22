import sys,unittest,struct,zlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from umi_touch_trace import decode_touch
from check_umi_touch import log_budget

def row(tag,n,f,v):
 crc=zlib.crc32(struct.pack('<BBQ',n,f,v))
 return f'\nUMI {tag} {n:02x} {f:02x} {v:016x} {crc:08x}\n'.encode()
class TouchTraceTests(unittest.TestCase):
 def test_complete_raw(self):
  data=b''.join(row('TCHX',1,i,1 if i in (0,1) else 0) for i in range(16))
  r=decode_touch(data)['raw_events'][0];self.assertTrue(r['complete']);self.assertEqual(r['raw'],'00 00 00 00 00 00 00 00')
 def test_partial_is_not_success(self):
  r=decode_touch(row('TOUC',1,0,1))['snapshots'][0]
  self.assertFalse(r['complete']);self.assertIsNone(r['hid_last_result']);self.assertIsNone(r['hid_starts'])
 def test_bad_crc(self):
  r=decode_touch(row('TOUC',1,0,1).replace(b'0000000000000001',b'0000000000000002'))
  self.assertEqual(r['crc_rejected_records'],1);self.assertFalse(r['snapshots'])
 def test_conflict(self):
  r=decode_touch(row('TOUC',1,0,1)+row('TOUC',1,0,2));self.assertEqual(r['conflicts'],[['TOUC',1]])
 def test_budget(self):
  b=log_budget();self.assertLess(b['log_bytes_max'],b['console_bytes'])
 def test_driver_stop_without_fifo_data(self):
  values=[1,1,0,0x103,0,0,90,0,0,0,0,0,0,0,0,0]
  r=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))['raw_events'][0]
  self.assertTrue(r['complete']);self.assertEqual(r['driver_stop_reason'],3)
  self.assertEqual(r['raw'],'00 00 00 00 00 00 00 00')
 def test_transport_timeout_has_own_schema(self):
  values=[2,1,51,1,7,6,9321,0,3,7,0,1,1,0,7400,1]
  r=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))
  self.assertFalse(r['raw_events']);e=r['transport_events'][0]
  self.assertTrue(e['complete']);self.assertEqual(e['phase'],7);self.assertEqual(e['byte_index'],6)
  self.assertEqual(e['max_clock_gap_us'],7400);self.assertEqual(e['retry'],1)
 def test_partial_transport_does_not_imply_retry(self):
  r=decode_touch(row('TCHX',1,0,2))['transport_events'][0]
  self.assertFalse(r['complete']);self.assertIsNone(r['retry']);self.assertIsNone(r['stop_result'])
 def test_recovery_has_own_schema(self):
  values=[3,1,51,0,9,700,2,3,7,5,0,1,0,1,0,1]
  r=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))
  self.assertFalse(r['raw_events']);self.assertFalse(r['transport_events'])
  e=r['recovery_events'][0];self.assertTrue(e['complete']);self.assertEqual(e['pulses'],9)
  self.assertEqual(e['lines_before'],2);self.assertEqual(e['lines_after'],3);self.assertEqual(e['retry'],1)
 def test_partial_recovery_cannot_claim_success(self):
  e=decode_touch(row('TCHX',1,0,3))['recovery_events'][0]
  self.assertFalse(e['complete']);self.assertIsNone(e['result']);self.assertIsNone(e['retry'])
 def test_sensing_has_own_schema(self):
  values=[4,1,60,1,4,0,53000,1,23,4000,4000,7,13000,13500,0,1]
  r=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))
  self.assertFalse(r['raw_events']);self.assertFalse(r['transport_events']);self.assertFalse(r['recovery_events'])
  e=r['sensing_events'][0];self.assertTrue(e['complete']);self.assertEqual(e['completed_commands'],4)
  self.assertEqual(e['irq_low_polls'],23);self.assertEqual(e['fifo_nonempty'],7)
 def test_partial_sensing_cannot_claim_initialization(self):
  e=decode_touch(row('TCHX',1,0,4))['sensing_events'][0]
  self.assertFalse(e['complete']);self.assertIsNone(e['completed_commands']);self.assertIsNone(e['failure'])
 def sys_page(self,n,offset,data):
  words=struct.unpack('<11Q',data.ljust(88,b'\0'))
  return b''.join(row('TCHX',n,i,v) for i,v in enumerate([5,n,0,offset,len(data),*words]))
 def sys_data(self):
  data=bytearray(208);data[0:2]=bytes([0xa5,1]);struct.pack_into('<HHH',data,16,0x128,0x4053,0x4b)
  data[28:31]=bytes([5,6,7]);data[32:35]=bytes([0xaa,0xbb,0xcc]);struct.pack_into('<HH',data,80,1080,2340)
  data[84:86]=bytes([32,18]);return bytes(data)
 def test_system_info_pages_and_offsets(self):
  data=self.sys_data();wire=b''.join(self.sys_page(n,offset,data[offset:offset+88]) for n,offset in enumerate([176,0,88],1))
  s=decode_touch(wire)['system_info'];self.assertTrue(s['complete']);self.assertTrue(s['valid'])
  self.assertEqual((s['fw'],s['svn'],s['config']),(0x128,0x4053,0x4b));self.assertEqual((s['cfg_afe'],s['cx_afe'],s['panel_afe']),(5,6,7))
  self.assertEqual((s['res_x'],s['res_y'],s['tx'],s['rx']),(1080,2340,32,18))
 def test_missing_system_info_page(self):
  s=decode_touch(self.sys_page(1,0,self.sys_data()[:88]))['system_info'];self.assertFalse(s['complete']);self.assertFalse(s['valid']);self.assertNotIn('fw',s)
 def test_system_info_conflict(self):
  data=self.sys_data();wire=self.sys_page(1,0,data[:88])+self.sys_page(2,0,b'X'+data[1:88])
  self.assertEqual(decode_touch(wire)['system_info']['reason'],'conflicting pages')
 def test_system_info_bad_range(self):
  p=decode_touch(self.sys_page(1,200,b'X'*16))['sysinfo_pages'][0];self.assertFalse(p['valid_range']);self.assertIsNone(p['data'])
 def test_partial_page_cannot_become_system_info(self):
  r=decode_touch(row('TCHX',1,0,5));self.assertIsNone(r['sysinfo_pages'][0]['data']);self.assertFalse(r['system_info']['valid'])
 def test_controller_metadata_and_irq_are_separate(self):
  values=[6,1,0,7,0,208,0x1fff,0,3,1,0xc3,2,0x4b0128,0x4053,0x070605,120000]
  r=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)));self.assertEqual(r['controller_checks'][0]['crc_error_bits'],3)
  self.assertFalse(r['system_info']['complete']) # Success flags cannot manufacture missing bytes.
  values=[7,2,60,3,4,5000,1,1,0xc3,0xc3,0,1,5004,5000,8,13000]
  r=decode_touch(b''.join(row('TCHX',2,i,v) for i,v in enumerate(values)));self.assertEqual(r['irq_events'][0]['transitions'],3)
 def test_controller_crc_event_classification(self):
  raw=struct.unpack('<Q',bytes([0xf3,0xa0,0,0,0,0,0,0]))[0]
  values=[1,1,0,3,raw,7,4,0,0,0,0,0,0,0,0,0]
  e=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))['raw_events'][0]
  self.assertEqual(e['controller_crc_error'],'cx_header')
 def test_signed_frame_sample_and_valid_mask(self):
  words=struct.unpack('<6Q',struct.pack('<24h',*range(-12,12)))
  values=[8,1,5,1,9,3,0x8d78,0x8878,0x88b6,*words,12000]
  r=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))
  self.assertFalse(r['raw_events']);e=r['frame_samples'][0]
  self.assertTrue(e['complete']);self.assertTrue(e['rolling_sample'])
  self.assertEqual(e['ms_nodes'],list(range(-12,-4)))
  self.assertEqual(e['ss_tx_nodes'],list(range(-4,4)))
  self.assertIsNone(e['ss_rx_nodes']) # A failed plane never becomes eight fabricated zeros.
 def test_partial_or_invalid_frame_mask_never_publishes_nodes(self):
  values=[8,1,5,1,0,8,0x8d78,0x8878,0x88b6,0,0,0,0,0,0,0]
  for wire in (row('TCHX',1,0,8),b''.join(row('TCHX',1,i,v) for i,v in enumerate(values))):
   e=decode_touch(wire)['frame_samples'][0]
   for plane in ('ms','ss_tx','ss_rx'):self.assertIsNone(e[plane+'_nodes'])
 def test_reset_bringup_power_reads_require_valid_bits(self):
  values=[9,1,0,2,1,0,10050,15,128,128,1800,512,3,512,3,1]
  e=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))['bringup_events'][0]
  self.assertTrue(e['supply_enabled']);self.assertTrue(e['supply_ready']);self.assertEqual(e['supply_mv'],1800)
  for mask in (0,4,8,16):
   values[7]=mask;e=decode_touch(b''.join(row('TCHX',1,i,v) for i,v in enumerate(values)))['bringup_events'][0]
   self.assertIsNone(e['supply_enabled']);self.assertIsNone(e['supply_ready']);self.assertIsNone(e['supply_mv'])
  e=decode_touch(row('TCHX',1,0,9))['bringup_events'][0];self.assertIsNone(e['supply_enabled']);self.assertIsNone(e['supply_mv'])
 def test_frozen_probe_metadata_and_failed_restore(self):
  values=[10,1,30,2,3,255,0,0,0,0,0,7,280000,1,1,0]
  frame=[8,2,30,2,0,7,0x8d78,0x8878,0x88b6,0,0,0,0,0,0,280000]
  wire=lambda tag,n,values:b''.join(row(tag,n,i,v) for i,v in enumerate(values))
  d=decode_touch(wire('TCHX',1,values)+wire('TCHX',2,frame));self.assertTrue(d['frame_probes'][0]['normal_mode_writes_succeeded'])
  self.assertFalse(d['frame_samples'][0]['rolling_sample']);self.assertEqual(d['frame_samples'][0]['sampling_mode'],'locked_frozen')
  values[10]=12;d=decode_touch(wire('TCHX',1,values));self.assertFalse(d['frame_probes'][0]['normal_mode_writes_succeeded'])
  e=decode_touch(row('TCHX',1,0,10))['frame_probes'][0];self.assertIsNone(e['normal_mode_writes_succeeded'])
if __name__=='__main__':unittest.main()
