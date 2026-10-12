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
    # Free-list reuse can wrap physical VPPNs inside one training batch.
    # Anchor at the minimum so the unsigned OLS samples do not underflow.
    old = '    uint64_t start_ppn = ppns[0];'
    assert text.count(old) == 1
    text = text.replace(old, old + '''
    for (int i = 1; i < len; i++)
        if (ppns[i] < start_ppn) start_ppn = ppns[i];''')
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
    text = text.replace(old, 'sg->inter = (double)b + start_ppn - (double)k * sg->slpa;')
    # An approximation may overshoot. Check before converting to uint64_t or
    # constructing a physical address; the read path then loads the real TP.
    anchor = 'static int segment_search_simple_segment('
    index = text.index(anchor)
    text = text[:index] + '''static uint64_t bounded_segment_prediction(simple_segment *sg,
                                            uint64_t off_lpn,
                                            learned_mgmt *lm)
{
    double predicted = (double)sg->k * off_lpn + sg->inter;
    if (!isfinite(predicted) || predicted < 0 ||
        predicted >= lm->sp->page_total_cnt)
        return INVALID_VAL;
    return (uint64_t)predicted;
}

''' + text[index:]
    text = text.replace('*ppn = off_lpn*sg->k + sg->inter;',
                        '*ppn = bounded_segment_prediction(sg, off_lpn, lm);')
    text = text.replace('*ppn = crb_sm->k * off_lpn + crb_sm->inter;',
                        '*ppn = bounded_segment_prediction(crb_sm, off_lpn, lm);')
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
    # Read multiplicity is the paper's LeaFTL hit definition. Count actual
    # translation reads and exclude RAM-buffer reads from single-flash hits.
    a = text.index('static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)\n{')
    b = text.index('static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)\n{', a)
    read = text[a:b]
    read = read.replace('        ssd->stat.access_cnt++;',
                        '        ssd->stat.access_cnt++;\n        sublat = 0;\n        vppn = INVALID_VAL;\n        memset(&lru_en, 0, sizeof(lru_en));\n        lru_en.len = -1;')
    read = read.replace('for (int i = 0; i < 1; i++)',
                        'for (int i = 0; i < ssd->buffer_pointer; i++)')
    read = read.replace('            ssd->stat.single_read++;\n            maxlat',
                        '            maxlat')
    a1 = read.index('                cmt_ent->sm.inter = lru_en.inter;')
    b1 = read.index('\n\n                // * cache miss', a1)
    read = read[:a1] + '                cmt_ent->sm = lru_en;' + read[b1:]
    read = read.replace('                translation_page_read(ssd, req, &ppa);',
                        '                translation_page_read(ssd, req, &ppa);\n                read_cnt++;')
    old = '        if (vppn == INVALID_VAL) {'
    assert read.count(old) == 1
    read = read.replace(old, '''        if (vppn == INVALID_VAL || vppn >= spp->tt_pgs) {
            struct ppa actual = get_maptbl_ent(ssd, lpn);
            struct ppa tp = get_gtd_ent(ssd, tvpn);
            if (mapped_ppa(&actual) && valid_ppa(ssd, &actual) &&
                mapped_ppa(&tp) && valid_ppa(ssd, &tp)) {
                if (read_cnt == 0) {
                    translation_page_read(ssd, req, &tp);
                    read_cnt++;
                    old_lun = get_lun(ssd, &tp);
                }
                vppn = ppa2vppn(ssd, &actual);
            }
        }
        if (vppn == INVALID_VAL) {''')
    text = text[:a] + read + text[b:]
    path.write_text(text)
    header = root / 'femu/bbssd/leaftl.h'
    old_header = (backup / '1-leaftl.h').read_text()
    assert old_header.count('uint64_t inter;') == 1
    new_header = old_header.replace('uint64_t inter;', 'double inter;')
    header.write_text(new_header)
    bbfile = root / 'femu/bbssd/bb.c'
    old_bb = (backup / '2-bb.c').read_text()
    new_bb = old_bb.replace('" erase_energy=%.3Lf\\n",',
                            '" erase_energy=%.3Lf single_read=%d double_read=%d triple_read=%d\\n",')
    assert new_bb != old_bb
    new_bb = new_bb.replace('st->write_joule, st->erase_joule);',
                            'st->write_joule, st->erase_joule, st->single_read, st->double_read, st->triple_read);')
    bbfile.write_text(new_bb)
    (root / 'mechanism-repairs.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), text.splitlines(True),
        fromfile='a/bbssd/leaftl.c', tofile='b/bbssd/leaftl.c')) + ''.join(difflib.unified_diff(
        old_header.splitlines(True), new_header.splitlines(True),
        fromfile='a/bbssd/leaftl.h', tofile='b/bbssd/leaftl.h')) + ''.join(difflib.unified_diff(
        old_bb.splitlines(True), new_bb.splitlines(True),
        fromfile='a/bbssd/bb.c', tofile='b/bbssd/bb.c')))
    print(folder, hashlib.sha256(path.read_bytes()).hexdigest())
