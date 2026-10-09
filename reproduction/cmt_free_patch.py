"""Opt-in free CMT queue diagnostics. Does not change allocation or eviction."""
from pathlib import Path

def apply(c):
    def once(old, new):
        nonlocal c
        assert c.count(old) == 1, (old, c.count(old))
        c = c.replace(old, new, 1)
    code = Path(__file__).with_name('cmt_free_diag.inc').read_text()
    once('#include "util.h"', '#include "util.h"\n' + code)
    once('    ftl_assert(cm->free_cmt_entry_cnt == cm->tt_entries);',
         '    ftl_assert(cm->free_cmt_entry_cnt == cm->tt_entries);\n    cf_init(ssd);')
    once('    insert_cmt_hashtable(ht, cmt_entry);\n}',
         '    insert_cmt_hashtable(ht, cmt_entry);\n    cf_alloc(ssd);\n}')
    once('    return tpnode_evict_flag;\n}',
         '    cf_return(ssd);\n    return tpnode_evict_flag;\n}')
    once('        maxlat = (curlat > maxlat) ? curlat : maxlat;',
         '        maxlat = (curlat > maxlat) ? curlat : maxlat;\n        cf_write(ssd);')
    once('void gc_diag_reset(struct ssd *ssd)\n{',
         'void gc_diag_reset(struct ssd *ssd)\n{\n    cf_reset(ssd);')
    once('void gc_diag_report(struct ssd *ssd)\n{',
         'void gc_diag_report(struct ssd *ssd)\n{\n    cf_report(ssd);')
    return c
