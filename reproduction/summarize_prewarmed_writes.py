#!/usr/bin/env python3
"""Keep post-read write observations in an explicitly separate cohort."""
import configparser
import csv
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
original = json.loads((HERE/'paper-summary.json').read_text())
assert not any(c.get('validation_error') for c in original['cases'] if not c['diagnostic_only'])
reads = [r for r in original['measurements'] if r['pattern'].endswith('read') and not r['diagnostic_only']]
assert len(reads)==30, 'Finish all validated read windows before final supplemental plots'
rows = [dict(r, cohort='formal prewarmed reads') for r in reads]
folders = {r['directory'] for r in reads}
assert len(folders)==10
for folder in sorted(folders):
    folder=Path(folder)
    method=json.loads((folder/'method.json').read_text())
    assert method['status']=='complete' and method['prewarm_passes']==6
    assert any(c['directory']==str(folder) and c.get('validated_raw_files')
               for c in original['cases']), 'Formal read case was not validated'
    probe=method['steady_write_probe']
    assert method['steady_write_probe_environment']['nvme_io_timeout_seconds']==method['guest']['formal_nvme_io_timeout_seconds']==30
    assert probe['diagnostic_only'] and probe['repeat_indices']==[1,2,3]
    assert len(probe['metrics'])==3
    assert probe['pattern']==('randwrite' if method['pattern']=='randread' else 'write')
    assert probe['prewarm_passes']==6 and probe['runtime_seconds']==60
    jobfile=configparser.ConfigParser();jobfile.read(folder/'steady-write.fio')
    assert jobfile['global']['rw']==probe['pattern']
    assert jobfile['global']['bs']=='4k' and jobfile['global']['runtime']=='60'
    assert jobfile['global']['ioengine']=='psync' and jobfile['global']['direct']=='1'
    assert jobfile['global']['time_based']=='1'
    assert len(jobfile.sections())==65
    for i in range(64):
        job=jobfile[f'job{i:02d}']; region=476*1024**2
        assert int(job['offset'])==i*region and int(job['size'])==region
        assert int(job['filesize'])==(i+1)*region
    for repeat,metric in zip(probe['repeat_indices'],probe['metrics']):
        data=json.loads((folder/f'steady-write{repeat}.json').read_text());jobs=data['jobs']
        assert all(j['error']==0 for j in jobs)
        total=sum(j['write']['io_bytes'] for j in jobs)
        assert total>0 and total%4096==0
        count=sum(j['write']['total_ios'] for j in jobs)
        assert count*4096==total, 'FIO page/operation count'
        assert all(j['write']['runtime']>=60000 for j in jobs), 'Incomplete 60s window'
        access=int(metric['access'])
        cmt,model,miss=(int(metric[k]) for k in ('cmt_hit','model_hit','cmt_miss'))
        if method['variant']=='learnedftl': assert access==cmt+model+miss
        # The cohort is supplemental, explicitly labeled in every row and
        # figure. The original method still marks these probes diagnostic.
        row={'variant':method['variant'],'pattern':probe['pattern'],'repeat':repeat,
             'diagnostic_only':False,
             'throughput_MB_s':sum(j['write']['io_bytes']*1000/j['write']['runtime'] for j in jobs)/1e6,
             'host_io_bytes':total,
             'fio_runtime_seconds':max(j['write']['runtime'] for j in jobs)/1000,
             'max_io_latency_ms':max(j['write']['lat_ns']['max'] for j in jobs)/1e6,
             'mean_latency_us':sum(j['write']['lat_ns']['mean']*j['write']['total_ios'] for j in jobs)/count/1000,
             'cmt_hit_ratio':cmt/access if access else None,
             'model_hit_ratio':model/access if access else None,
             'translation_miss_ratio':miss/access if access else None,
             'single_flash_read_ratio':int(metric['single_read'])/access if 'single_read' in metric and access else None,
             'double_flash_read_ratio':int(metric['double_read'])/access if 'double_read' in metric and access else None,
             'triple_flash_read_ratio':int(metric['triple_read'])/access if 'triple_read' in metric and access else None,
             'mapping_accesses':access,'gc_counter':int(metric['gc']),
             'nand_writes':int(metric['writes']),
             'write_amplification':int(metric['writes'])/(total/4096),
             'energy_model_units':sum(float(metric[k]) for k in ('read_energy','write_energy','erase_energy')),
             'binary_sha256':method['build']['sha256'],'directory':str(folder),
             'cohort':'supplemental consecutive prewarmed writes after read measurements'}
        rows.append(row)
assert len(rows)==60
payload={'notes':original['notes']+[
    'SUPPLEMENT: read rows unchanged; write rows are consecutive post-read windows on matching six-pass-prewarmed devices.',
    'These observations do not replace the independent fresh-write cohort. Consecutive windows change the device state.',
    'Six passes are a preconditioning procedure, not proof that steady state has been reached.'],
    'cases':original['cases'],'measurements':rows}
(HERE/'paper-prewarmed-summary.json').write_text(json.dumps(payload,indent=2,ensure_ascii=False)+'\n')
with (HERE/'paper-prewarmed-summary.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print('Supplemental write observations:',30)
