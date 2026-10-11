#!/usr/bin/env python3
"""Repair concrete source defects in the isolated author LeaFTL baseline.

Keep original source/build/validation evidence before changing either geometry.
This does not tune parameters or change LearnedFTL's production source.
"""
import difflib
import hashlib
import json
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent
for folder in ('variants', 'variants-diagnostic'):
    root = HERE / folder / 'leaftl'
    backup = root / 'before-mechanism-repairs'
    if not backup.exists():
        backup.mkdir()
        manifest = json.loads((root / 'build-manifest.json').read_text())
        shutil.copy2(root / 'build-manifest.json', backup / 'build-manifest.json')
        shutil.copy2(manifest['binary'], backup / 'qemu-binary')
        for i, (filename, digest) in enumerate(manifest['sources'].items()):
            assert hashlib.sha256(Path(filename).read_bytes()).hexdigest() == digest
            shutil.copy2(filename, backup / (str(i) + '-' + Path(filename).name))
        if (root / 'validation.json').exists():
            shutil.copy2(root / 'validation.json', backup / 'validation.json')
    path = root / 'femu/bbssd/leaftl.c'
    original = text = (backup / '0-leaftl.c').read_text()
    text = text.replace('    printf("DO_GC\\n");\n    ssd->stat.gc_cnt++;\n', '')
    old = '    ppa.g.blk = victim_line->id;'
    assert text.count(old) == 1
    text = text.replace(old, '    ssd->stat.gc_cnt++;\n' + old)
    text = text.replace('sp->page_total_cnt = 8388608;', 'sp->page_total_cnt = ssd->sp.tt_pgs;')
    old = '    LeastSquare(off_x, off_y, len, &k, &b);'
    assert text.count(old) == 1
    text = text.replace(old, '''    /* A singleton has a constant, exact mapping; OLS would divide by zero. */
    if (len == 1) {
        k = 0;
        b = 0;
    } else {
        LeastSquare(off_x, off_y, len, &k, &b);
    }''')
    start = text.index('static void GC_flush(struct ssd *ssd, uint64_t* buffer_lpns, int pointer) {')
    end = text.index('\nvoid count_segments(', start)
    body = text[start:end]
    assert body.count('    int threshold = lm->sp->threshold;') == 2
    body = body.replace('    int threshold = lm->sp->threshold;\n', '')
    body = body.replace('uint64_t gtd_to_update[gtd_cnt];', 'bool gtd_to_update[gtd_cnt];')
    body = body.replace('int gtd_index = ssd->buffer_lpns[i]/ssd->sp.ents_per_pg;\n        gtd_to_update[gtd_index] = ssd->buffer_lpns[i];',
                        'int gtd_index = lpns[i] / sp->ents_per_pg;\n        gtd_to_update[gtd_index] = true;')
    body = body.replace('uint64_t tvpn = gtd_to_update[i] / sp->ents_per_pg;', 'uint64_t tvpn = i;')
    assert body.count('gcr.cmd = NAND_READ;') == 2
    body = body.replace('gcr.cmd = NAND_READ;', 'gcr.cmd = NAND_WRITE;')
    # Both old loops dropped the final sample and singleton batches. Route them
    # through one half-open range implementation, including the final boundary.
    for tail in ('\n\n}', '\n\n    return maxlat;'):
        first = body.index('    int last_pointer = 0;')
        last = body.index(tail, first)
        body = body[:first] + '    train_mapping_batch(lm, lpns, vppn, buffer_size);' + body[last:]
    body = body.replace('    const int buffer_size = pointer;',
                        '    const int buffer_size = pointer;\n    if (buffer_size == 0) return;')
    helper = '''/* Train every sample once; never cross a LeaFTL mapping-page boundary. */
static void train_mapping_batch(learned_mgmt *lm, uint64_t *lpns,
                                uint64_t *ppns, int count)
{
    int first = 0;
    for (int end = 1; end <= count; end++) {
        bool boundary = end == count;
        if (!boundary) {
            boundary = lpns[end] - lpns[end - 1] > lm->sp->threshold ||
                       end - first >= TRAIN_LEN ||
                       lpns[end] / lm->sp->mapping_per_page !=
                           lpns[end - 1] / lm->sp->mapping_per_page;
        }
        if (boundary) {
            flush_lpn_ppn_mappings(lm, lpns + first, ppns + first, end - first);
            for (int i = first; i < end; i++) lm->bitmap[lpns[i]] = 1;
            first = end;
        }
    }
}

'''
    text = text[:start] + helper + body + text[end:]
    # The stored segment length is inclusive. CRB records mark live mappings.
    old = 'for (int i = sm->slpa; i < sm->slpa+sm->len; i++)'
    assert text.count(old) == 1
    text = text.replace(old, 'for (int i = sm->slpa; i <= sm->slpa+sm->len; i++)')
    old = 'valid_lpns[tmp->record[i]-slpa] = 0;'
    assert text.count(old) == 1
    text = text.replace(old, 'valid_lpns[tmp->record[i]-slpa] = 1;')
    old = 'sg->inter = b + start_ppn;'
    assert text.count(old) == 1
    text = text.replace(old, 'sg->inter = b + start_ppn - k * sg->slpa;')
    # Search CRB entries using the same prediction/cached-segment contract.
    text = text.replace('        int flag = 0;\n        while (tmp)',
                        '        while (tmp)')
    text = text.replace('            lru_en = crb_sm;',
                        '            *lru_en = *crb_sm;')
    a = text.index('    } else {', text.index('static segment* segment_insert_simple_segment('))
    b = text.index('\n    return NULL;', a)
    text = text[:a] + """    } else {
        /* Insert a disjoint interval, or place an overlapping batch in a
         * newer level so that the previous mappings remain available. */
        simple_segment **link = &sg->segment_head;
        while (*link && get_upper_lpn_bound(*link) < start_lpn)
            link = &(*link)->next;
        if (*link && get_lower_lpn_bound(*link) <= end_lpn)
            return sg;
        new_seg->next = *link;
        *link = new_seg;
    }
""" + text[b:]
    # Only currently buffered LPNs suppress a new physical write. Old slots
    # beyond buffer_pointer are stale. Reset duplicate detection per LPN.
    a = text.index('static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)\n{')
    body = text[a:]
    body = body.replace('        st->write_cnt++;',
                        '        st->write_cnt++;\n        update_in_buffer = false;')
    body = body.replace('                set_rmap_ent(ssd, INVALID_LPN, &ppa);',
                        '                set_rmap_ent(ssd, INVALID_LPN, &ppa);\n                ssd->lem->bitmap[lpn] = 0;')
    body = body.replace('i < ssd->sp.buffer_size;', 'i < ssd->buffer_pointer;')
    text = text[:a] + body
    path.write_text(text)
    (root / 'mechanism-repairs.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), text.splitlines(True),
        fromfile='a/bbssd/leaftl.c', tofile='b/bbssd/leaftl.c')))
    print(folder, hashlib.sha256(path.read_bytes()).hexdigest())
