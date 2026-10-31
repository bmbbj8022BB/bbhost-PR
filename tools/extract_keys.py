#!/usr/bin/env python3
"""Lists every English key used with tr()/trl()/tf() in the given C++ files."""
import re, sys, json
def lits(s, i):
    """string literals starting at s[i] (adjacent ones joined); returns (value, end)"""
    out, j = [], i
    while True:
        if s[j] != '"': break
        j += 1; buf = []
        while s[j] != '"':
            if s[j] == '\\':
                c = s[j+1]; buf.append({'n':'\n','t':'\t','"':'"','\\':'\\',"'":"'"}.get(c, c)); j += 2
            else: buf.append(s[j]); j += 1
        j += 1; out.append(''.join(buf))
        k = j
        while k < len(s) and s[k] in ' \t\r\n': k += 1
        if k < len(s) and s[k] == '"': j = k; continue
        break
    return ''.join(out), j
def keys(path):
    s = open(path, encoding='utf-8').read()
    res = []
    for m in re.finditer(r'\b(?:i18n::)?(tr|trl|tf|trs)\(\s*(?=")', s):
        v, _ = lits(s, m.end())
        if m.group(1) == 'trl': v = v.split('##')[0]
        if v and v not in ('%s',): res.append(v)
    return res
if __name__ == '__main__':
    allk = []
    for p in sys.argv[1:]:
        for k in keys(p):
            if k not in allk: allk.append(k)
    json.dump(allk, sys.stdout, ensure_ascii=False, indent=0)
