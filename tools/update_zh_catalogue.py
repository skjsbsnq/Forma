#!/usr/bin/env python3
"""Regenerate the C++ display catalogue after editing applications/common/zh_CN.json."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
catalogue = root / 'applications/common/zh_CN.json'
header = root / 'applications/common/zh_cn.hpp'
messages = json.loads(catalogue.read_text())
entries = '\n'.join(
    '        {' + json.dumps(key, ensure_ascii=False) + ', '
    + json.dumps(value, ensure_ascii=False) + '},'
    for key, value in messages.items()
)
text = header.read_text()
start_marker = 'static const std::unordered_map<std::string_view, std::string_view> messages{\n'
start = text.index(start_marker) + len(start_marker)
end = text.index('\n    };', start)
header.write_text(text[:start] + entries + text[end:])
