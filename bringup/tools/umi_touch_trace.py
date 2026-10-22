"""Decode CRC-protected FTS wire events and end-to-end HID observations."""
import re,struct,zlib
FIELDS=('version','stage','failure','raw_events','point_events','controller_resets','chip_errors','transport_errors','last_transport_result','last_ack','last_raw',
 'contacts','sequence','queue_overflows','hid_stage','hid_load_result','hid_match_result','hid_resource','hid_starts','hid_attempts','hid_last_result',
 'hid_reports','polls','queued_frames','edge_installed','edge_contacts','edge_from_edge','edge_skipped','frame_sequence','raw_records','seconds','hid_start_failures')
RAW_FIELDS=('version','record','seconds','decode_result','raw','ack','stage','frame_sequence','contacts','contact_mask','id','x','y','transport_errors','queue_overflows','hid_reports')
BUS_FIELDS=('version','record','seconds','result','phase','byte_index','elapsed_us','stop_result','line_levels','ack','geni_status','pins_power_valid','consecutive_failures','contacts_cancelled','max_clock_gap_us','retry')
RECOVERY_FIELDS=('version','record','seconds','result','pulses','elapsed_us','lines_before','lines_after','failed_phase','failed_byte','irq_level','pins_power_valid','geni_status','consecutive_failures','contacts_cancelled','retry')
SENSING_FIELDS=('version','record','seconds','sensing_runs','completed_commands','failure','sensing_elapsed_us','irq_level','irq_low_polls','irq_high_polls','fifo_empty','fifo_nonempty','last_transaction_us','max_read_us','irq_gpio_ctl','irq_gpio_io')
SYS_PAGE_FIELDS=('version','record','seconds','offset','length')+tuple('word'+str(i) for i in range(11))
CHECK_FIELDS=('version','record','seconds','flags','sysinfo_result','sysinfo_bytes','sysinfo_chunk_mask','crc_result','crc_raw','irq_ctl_before','irq_ctl_after','irq_transitions','config_fw','svn','afe_versions','elapsed_us')
IRQ_FIELDS=('version','record','seconds','transitions','low_polls','high_polls','level','ctl_before','ctl_expected','ctl_current','geni_status','power_pins_valid','polls','fifo_empty','fifo_nonempty','max_read_us')
FRAME_FIELDS=('version','record','seconds','sample','result','valid_mask','ms_address','ss_tx_address','ss_rx_address','ms_word0','ms_word1','ss_tx_word0','ss_tx_word1','ss_rx_word0','ss_rx_word1','elapsed_us')
BRINGUP_FIELDS=('version','record','seconds','phase','reset_count','result','elapsed_us','pmic_valid_mask','supply_enable_raw','supply_status_raw','supply_mv_raw','reset_ctl','reset_io','avdd_ctl','avdd_io','irq_io')
PROBE_FIELDS=('version','record','seconds','sample','requested_mode','flags','start_result','off_result','frame_result','restore_on_result','restore_gesture_result','valid_mask','elapsed_us','restore_allowed','power_pins_valid','geni_status')
EXTRA_SCHEMAS={5:('sysinfo_pages',SYS_PAGE_FIELDS),6:('controller_checks',CHECK_FIELDS),7:('irq_events',IRQ_FIELDS),8:('frame_samples',FRAME_FIELDS),9:('bringup_events',BRINGUP_FIELDS),10:('frame_probes',PROBE_FIELDS)}
def assemble_sysinfo(pages):
    data=bytearray(208);known=set()
    for p in pages:
        if not p.get('complete') or not p.get('valid_range') or p.get('data') is None:
            return dict(complete=False,valid=False,reason='missing or invalid page')
        for offset,value in enumerate(bytes.fromhex(p['data']),p['offset']):
            if offset in known and data[offset]!=value:return dict(complete=False,valid=False,reason='conflicting pages')
            data[offset]=value;known.add(offset)
    if len(known)!=208:return dict(complete=False,valid=False,reason='incomplete 208-byte coverage',bytes_covered=len(known))
    out=dict(complete=True,valid=data[0]==0xa5 and data[1]==1,signature=data[0],data_id=data[1],data=data.hex(' '))
    if not out['valid']:return out
    for key,offset in dict(chip0=10,chip1=14,fw=16,svn=18,config=20,cx=24,res_x=80,res_y=82,ms_raw=136,ss_tx_raw=144,ss_rx_raw=152).items():
        out[key]=struct.unpack_from('<H',data,offset)[0]
    out.update(cfg_afe=data[28],cx_afe=data[29],panel_afe=data[30],protocol=data[31],tx=data[84],rx=data[85])
    return out
def decode_touch(data):
    groups={};valid=bad=0
    for row in re.findall(rb'\nUMI (TOUC|TCHX) ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{16}) ([0-9a-f]{8})\n',data):
        tag=row[0].decode();event,field,value,crc=(int(x,16) for x in row[1:]);count=32 if tag=='TOUC' else 16
        if not 1<=event<=255 or field>=count or zlib.crc32(struct.pack('<BBQ',event,field,value))!=crc:bad+=1;continue
        valid+=1;groups.setdefault((tag,event),{}).setdefault(field,set()).add(value)
    out=dict(snapshots=[],raw_events=[],transport_events=[],recovery_events=[],sensing_events=[],sysinfo_pages=[],controller_checks=[],irq_events=[],frame_samples=[],bringup_events=[],frame_probes=[],crc_valid_records=valid,crc_rejected_records=bad,conflicts=[])
    for (tag,event),fields in sorted(groups.items()):
        if any(len(v)!=1 for v in fields.values()):out['conflicts'].append([tag,event]);continue
        v={i:next(iter(x)) for i,x in fields.items()};names=FIELDS if tag=='TOUC' else RAW_FIELDS
        bus=tag=='TCHX' and v.get(0)==2
        recovery=tag=='TCHX' and v.get(0)==3
        sensing=tag=='TCHX' and v.get(0)==4
        extra=EXTRA_SCHEMAS.get(v.get(0)) if tag=='TCHX' else None
        if extra:names=extra[1]
        elif sensing:names=SENSING_FIELDS
        elif recovery:names=RECOVERY_FIELDS
        elif bus:names=BUS_FIELDS
        elif v.get(0)!=1:continue
        r=dict(record=event,complete=len(v)==len(names),missing_fields=sorted(set(range(len(names)))-set(v)))
        r.update({name:v.get(i) for i,name in enumerate(names)})
        if extra:
            if v[0]==5:
                r['valid_range']=r['offset'] is not None and r['length'] is not None and 0<=r['offset']<208 and 0<r['length']<=88 and r['offset']+r['length']<=208
                r['data']=b''.join(struct.pack('<Q',v[i]) for i in range(5,16))[:r['length']].hex(' ') if r['complete'] and r['valid_range'] else None
            if v[0]==6 and r['complete'] and r['crc_result']==0 and r['flags']&4 and 0<=r['crc_raw']<=255:
                r['crc_error_bits']=r['crc_raw']&3
            if v[0]==8:
                r['rolling_sample']=True
                mask=r['valid_mask'];r['valid_mask_range']=mask is not None and 0<=mask<=7
                for i,plane in enumerate(('ms','ss_tx','ss_rx')):
                    r[plane+'_nodes']=list(struct.unpack('<8h',struct.pack('<QQ',r[plane+'_word0'],r[plane+'_word1']))) if r['complete'] and r['valid_mask_range'] and mask&(1<<i) else None
            if v[0]==9:
                mask=r['pmic_valid_mask'];valid=r['complete'] and mask is not None and 0<=mask<=15
                r['supply_enabled']=bool(r['supply_enable_raw']&0x80) if valid and mask&1 else None
                r['supply_ready']=bool(r['supply_status_raw']&0x80) if valid and mask&2 else None
                r['supply_mv']=r['supply_mv_raw'] if valid and mask&12==12 else None
            if v[0]==10:
                r['normal_mode_writes_succeeded']=(r['flags']&48==48 and r['restore_on_result']==r['restore_gesture_result']==0) if r['complete'] else None
            out[extra[0]].append(r);continue
        if bus or recovery or sensing:
            out['sensing_events' if sensing else 'recovery_events' if recovery else 'transport_events'].append(r);continue
        raw='last_raw' if tag=='TOUC' else 'raw'
        if r[raw] is not None:r[raw]=struct.pack('<Q',r[raw]).hex(' ')
        if tag=='TCHX' and r['decode_result'] is not None and r['decode_result']&0x100:
            r['driver_stop_reason']=r['decode_result']&0xff
        if tag=='TCHX' and r['decode_result']==5:r['waiting_for_ready']=True
        if tag=='TCHX' and r[raw] is not None:
            e=bytes.fromhex(r[raw]);crc_errors={0x20:'config_header',0x21:'config',0x22:'panel_header',0x23:'panel',0xa0:'cx_header',0xa1:'cx',0xa5:'cx_sub_header',0xa6:'cx_sub'}
            if e[0]==0xf3 and e[1] in crc_errors:r['controller_crc_error']=crc_errors[e[1]]
        out['snapshots' if tag=='TOUC' else 'raw_events'].append(r)
    out['system_info']=assemble_sysinfo(out['sysinfo_pages'])
    for p in out['frame_probes']:
        if not p['complete'] or p['requested_mode'] not in (0,3) or not p['flags']&4 or p['off_result']!=0:continue
        for f in out['frame_samples']:
            if f['sample']==p['sample']:
                f['rolling_sample']=False;f['sampling_mode']='normal_frozen' if p['requested_mode']==0 else 'locked_frozen'
    return out
