# Third-party notices

UE-AssetDoc is licensed under the Apache License, Version 2.0 (see [LICENSE](LICENSE) and [NOTICE](NOTICE)). This file lists what else is involved.

## Unreal Engine

The plugin is built against and runs inside Unreal Engine, which is not part of this repository. It requires a copy of Unreal Engine that you obtain under the Unreal Engine End User License Agreement from Epic Games, Inc. This repository contains no Unreal Engine source code or content and does not redistribute any. "Unreal" and "Unreal Engine" are trademarks or registered trademarks of Epic Games, Inc. This project is not affiliated with or endorsed by Epic Games. The engine plugins the plugin depends on (Python Editor Script Plugin, Enhanced Input, StateTree, Niagara, MetaSound, IK Rig, Control Rig and others) ship with the engine and stay under the engine licence.

## Python

The Python code under `Content/Python/` uses the Python standard library and the `unreal` module of the editor only. It has no third-party Python dependencies.

## npm packages (MCP stdio bridge)

The `ue-assetdoc` stdio bridge in `MCP/` depends on one package directly, `@modelcontextprotocol/sdk`, and through it on the packages below. The list is the production closure recorded in `MCP/package-lock.json` (93 packages; `content-type` is installed in two versions). The bridge is an optional component for MCP clients that only start stdio servers; the plugin itself does not need Node.js. The packages are installed by `npm install` and are not copied into this repository. The licence texts are in each package, under `MCP/node_modules/<name>/` after installation.

| Package | Version | Licence | Copyright |
|---|---|---|---|
| `@hono/node-server` | 2.1.5 | MIT | Copyright (c) 2022 - present, Yusuke Wada and Hono contributors |
| `@modelcontextprotocol/sdk` | 1.32.1 | MIT | Copyright (c) 2024 Anthropic, PBC |
| `accepts` | 2.0.0 | MIT | Copyright (c) 2014 Jonathan Ong |
| `ajv` | 8.20.0 | MIT | Copyright (c) 2015-2021 Evgeny Poberezkin |
| `ajv-formats` | 3.0.1 | MIT | Copyright (c) 2020 Evgeny Poberezkin |
| `body-parser` | 2.3.0 | MIT | Copyright (c) 2014 Jonathan Ong |
| `bytes` | 3.1.2 | MIT | Copyright (c) 2012-2014 TJ Holowaychuk |
| `call-bind-apply-helpers` | 1.0.2 | MIT | Copyright (c) 2024 Jordan Harband |
| `call-bound` | 1.0.4 | MIT | Copyright (c) 2024 Jordan Harband |
| `content-disposition` | 1.0.1 | MIT | Copyright (c) 2014-2017 Douglas Christopher Wilson |
| `content-type` | 1.0.5, 2.1.0 | MIT | Copyright (c) 2015 Douglas Christopher Wilson |
| `cookie` | 0.7.2 | MIT | Copyright (c) 2012-2014 Roman Shtylman |
| `cookie-signature` | 1.2.2 | MIT | Copyright (c) 2012–2024 LearnBoost and other contributors |
| `cors` | 2.8.6 | MIT | Copyright (c) 2013 Troy Goode |
| `cross-spawn` | 7.0.6 | MIT | Copyright (c) 2018 Made With MOXY Lda |
| `debug` | 4.4.3 | MIT | Copyright (c) 2014-2017 TJ Holowaychuk |
| `depd` | 2.0.0 | MIT | Copyright (c) 2014-2018 Douglas Christopher Wilson |
| `dunder-proto` | 1.0.1 | MIT | Copyright (c) 2024 ECMAScript Shims |
| `ee-first` | 1.1.1 | MIT | Copyright (c) 2014 Jonathan Ong |
| `encodeurl` | 2.0.0 | MIT | Copyright (c) 2016 Douglas Christopher Wilson |
| `es-define-property` | 1.0.1 | MIT | Copyright (c) 2024 Jordan Harband |
| `es-errors` | 1.3.0 | MIT | Copyright (c) 2024 Jordan Harband |
| `es-object-atoms` | 1.1.2 | MIT | Copyright (c) 2024 Jordan Harband |
| `escape-html` | 1.0.3 | MIT | Copyright (c) 2012-2013 TJ Holowaychuk |
| `etag` | 1.8.1 | MIT | Copyright (c) 2014-2016 Douglas Christopher Wilson |
| `eventsource` | 3.0.7 | MIT | Copyright (c) EventSource GitHub organisation |
| `eventsource-parser` | 3.0.6 | MIT | Copyright (c) 2025 Espen Hovlandsdal |
| `express` | 5.2.1 | MIT | Copyright (c) 2009-2014 TJ Holowaychuk |
| `express-rate-limit` | 8.7.1 | MIT | Copyright 2023 Nathan Friedly, Vedant K |
| `fast-deep-equal` | 3.1.3 | MIT | Copyright (c) 2017 Evgeny Poberezkin |
| `fast-uri` | 3.1.9 | BSD-3-Clause | Copyright (c) 2011-2021, Gary Court |
| `finalhandler` | 2.1.1 | MIT | Copyright (c) 2014-2022 Douglas Christopher Wilson |
| `forwarded` | 0.2.0 | MIT | Copyright (c) 2014-2017 Douglas Christopher Wilson |
| `fresh` | 2.0.0 | MIT | Copyright (c) 2012 TJ Holowaychuk |
| `function-bind` | 1.1.2 | MIT | Copyright (c) 2013 Raynos. |
| `get-intrinsic` | 1.3.0 | MIT | Copyright (c) 2020 Jordan Harband |
| `get-proto` | 1.0.1 | MIT | Copyright (c) 2025 Jordan Harband |
| `gopd` | 1.2.0 | MIT | Copyright (c) 2022 Jordan Harband |
| `has-symbols` | 1.1.0 | MIT | Copyright (c) 2016 Jordan Harband |
| `hasown` | 2.0.4 | MIT | Copyright (c) Jordan Harband and contributors |
| `hono` | 4.13.13 | MIT | Copyright (c) 2021 - present, Yusuke Wada and Hono contributors |
| `http-errors` | 2.0.1 | MIT | Copyright (c) 2014 Jonathan Ong |
| `iconv-lite` | 0.7.2 | MIT | Copyright (c) 2011 Alexander Shtuchkin |
| `inherits` | 2.0.4 | ISC | Copyright (c) Isaac Z. Schlueter |
| `ip-address` | 10.7.4 | MIT | Copyright (C) 2011 by Beau Gunderson |
| `ipaddr.js` | 1.9.1 | MIT | Copyright (C) 2011-2017 whitequark |
| `is-promise` | 4.0.0 | MIT | Copyright (c) 2014 Forbes Lindesay |
| `isexe` | 2.0.0 | ISC | Copyright (c) Isaac Z. Schlueter and Contributors |
| `jose` | 6.1.3 | MIT | Copyright (c) 2018 Filip Skokan |
| `json-schema-traverse` | 1.0.0 | MIT | Copyright (c) 2017 Evgeny Poberezkin |
| `json-schema-typed` | 8.0.2 | BSD-2-Clause | Original source code is copyright (c) 2019-2025 Remy Rylan |
| `math-intrinsics` | 1.1.0 | MIT | Copyright (c) 2024 ECMAScript Shims |
| `media-typer` | 1.1.0 | MIT | Copyright (c) 2014-2017 Douglas Christopher Wilson |
| `merge-descriptors` | 2.0.0 | MIT | Copyright (c) Jonathan Ong |
| `mime-db` | 1.54.0 | MIT | Copyright (c) 2014 Jonathan Ong |
| `mime-types` | 3.0.2 | MIT | Copyright (c) 2014 Jonathan Ong |
| `ms` | 2.1.3 | MIT | Copyright (c) 2020 Vercel, Inc. |
| `negotiator` | 1.0.0 | MIT | Copyright (c) 2012-2014 Federico Romero |
| `object-assign` | 4.1.1 | MIT | Copyright (c) Sindre Sorhus |
| `object-inspect` | 1.13.4 | MIT | Copyright (c) 2013 James Halliday |
| `on-finished` | 2.4.1 | MIT | Copyright (c) 2013 Jonathan Ong |
| `once` | 1.4.0 | ISC | Copyright (c) Isaac Z. Schlueter and Contributors |
| `parseurl` | 1.3.3 | MIT | Copyright (c) 2014 Jonathan Ong |
| `path-key` | 3.1.1 | MIT | Copyright (c) Sindre Sorhus |
| `path-to-regexp` | 8.4.2 | MIT | Copyright (c) 2014 Blake Embrey |
| `pkce-challenge` | 5.0.1 | MIT | Copyright (c) 2019 (holder not named in the licence text) |
| `proxy-addr` | 2.0.8 | MIT | Copyright (c) 2014-2016 Douglas Christopher Wilson |
| `qs` | 6.16.1 | BSD-3-Clause | Copyright (c) 2014, Nathan LaFreniere and other contributors |
| `range-parser` | 1.2.1 | MIT | Copyright (c) 2012-2014 TJ Holowaychuk |
| `raw-body` | 3.0.2 | MIT | Copyright (c) 2013-2014 Jonathan Ong |
| `require-from-string` | 2.0.2 | MIT | Copyright (c) Vsevolod Strukchinsky |
| `router` | 2.2.0 | MIT | Copyright (c) 2013 Roman Shtylman |
| `safer-buffer` | 2.1.2 | MIT | Copyright (c) 2018 Nikita Skovoroda |
| `send` | 1.2.1 | MIT | Copyright (c) 2012 TJ Holowaychuk |
| `serve-static` | 2.2.1 | MIT | Copyright (c) 2010 Sencha Inc. |
| `setprototypeof` | 1.2.0 | ISC | Copyright (c) 2015, Wes Todd |
| `shebang-command` | 2.0.0 | MIT | Copyright (c) Kevin Mårtensson |
| `shebang-regex` | 3.0.0 | MIT | Copyright (c) Sindre Sorhus |
| `side-channel` | 1.1.1 | MIT | Copyright (c) 2019 Jordan Harband |
| `side-channel-list` | 1.0.1 | MIT | Copyright (c) 2024 Jordan Harband |
| `side-channel-map` | 1.0.1 | MIT | Copyright (c) 2024 Jordan Harband |
| `side-channel-weakmap` | 1.0.2 | MIT | Copyright (c) 2019 Jordan Harband |
| `statuses` | 2.0.2 | MIT | Copyright (c) 2014 Jonathan Ong |
| `toidentifier` | 1.0.1 | MIT | Copyright (c) 2016 Douglas Christopher Wilson |
| `type-is` | 2.1.0 | MIT | Copyright (c) 2014 Jonathan Ong |
| `unpipe` | 1.0.0 | MIT | Copyright (c) 2015 Douglas Christopher Wilson |
| `vary` | 1.1.2 | MIT | Copyright (c) 2014-2017 Douglas Christopher Wilson |
| `which` | 2.0.2 | ISC | Copyright (c) Isaac Z. Schlueter and Contributors |
| `wrappy` | 1.0.2 | ISC | Copyright (c) Isaac Z. Schlueter and Contributors |
| `zod` | 4.3.6 | MIT | Copyright (c) 2025 Colin McDonnell |
| `zod-to-json-schema` | 3.25.1 | ISC | Copyright (c) 2020, Stefan Terdell |

Licences in use: 83 MIT, 7 ISC, 2 BSD-3-Clause, 1 BSD-2-Clause.

### Development dependencies (not distributed)

`typescript` (Apache-2.0), `@types/node` (MIT) and `undici-types` (MIT), the last one through `@types/node`, are needed only to compile and test the bridge (`npm run build`, `npm test`). They are not part of the production closure above and are not included in anything this project distributes.
