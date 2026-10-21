#!/usr/bin/env python3
"""Static scan (lower bound): which admitted generic-route factories contain modal/blocking UI calls
directly inside FactoryCreateNew / FactoryCreateFile / ConfigureProperties / PostCreate, and in which function.
Calls made through helpers in other files are NOT found (e.g. GroomFactory -> UE::Groom::BuildGroomDataflowAsset
-> SMessageDialog::ShowModal); those are found only by running the audit under the stall watchdog.
Usage: scan_modal_factories.py coverage-audit.json  (prints JSON; ENGINE_DIR env overrides the engine root)"""
import json, os, re, subprocess, sys
ENGINE = os.environ.get("ENGINE_DIR", "/Users/Shared/Epic Games/UE_5.7/Engine")
d = json.load(open(sys.argv[1]))
fac = {f['factory'].split('.')[-1] for f in d['factories'] if f['verdict'] == 'admitted'}
admitted = {f['factory'].split('.')[-1]: f['supported_class'] for f in d['factories'] if f['verdict'] == 'admitted'}
TOK = re.compile(r'ShowModal|SMessageDialog|FMessageDialog::Open|OpenFileDialog|SaveFileDialog|OpenDirectoryDialog|AddModalWindow|PromptUser|CreateModalDialog|FDesktopPlatformModule|SClassPickerDialog|PickClass|FSuppressableWarningDialog|EditObject\(|OpenEditorForAsset|OpenEditorForAssets')
FUNC = re.compile(r'^[\w:<>,\*&\s]*?\b(U\w+)::(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{', re.M)
roots = [ENGINE + "/Source", ENGINE + "/Plugins"]
files = subprocess.run(["grep", "-rlE", "--include=*.cpp", r"U\w+::(FactoryCreateNew|ConfigureProperties|PostCreate|FactoryCreateFile)", *roots], capture_output=True, text=True).stdout.splitlines()
out = {}
for path in files:
    src = open(path, errors='ignore').read()
    for m in FUNC.finditer(src):
        cls, fn = m.group(1), m.group(2)
        cls = cls[1:]
        if cls not in fac: continue
        i = m.end(); depth = 1
        while i < len(src) and depth:
            depth += (src[i] == '{') - (src[i] == '}'); i += 1
        body = src[m.end():i]
        toks = sorted(set(TOK.findall(body)))
        if fn in ('FactoryCreateNew', 'FactoryCreateFile', 'ConfigureProperties', 'PostCreate', 'CanCreateNew', 'ShouldShowInNewMenu') and toks:
            out.setdefault(cls, []).append({'function': fn, 'tokens': toks, 'file': path.replace(ENGINE + '/', 'Engine/'), 'supported_class': admitted[cls]})
print(json.dumps(out, indent=1))
