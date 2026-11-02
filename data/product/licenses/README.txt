Third-party licence files for this EpoCanva package.

The exact required files and byte digests are compiled into the product from
 data/manifests/product-package-files.inc. The staged product-dependencies.json
records source identities, linkage evidence and the selected licence branches.
The package manifest verifies every payload file; the licence directory being
nonempty is not sufficient.

librime-LICENSE.txt             librime 1.17.0, BSD-3-Clause
yaml-cpp-LICENSE.txt            yaml-cpp 0.8.0, MIT
leveldb-LICENSE.txt             LevelDB 1.23.0, BSD-3-Clause
marisa-trie-COPYING.md          marisa-trie 0.3.1, BSD-2-Clause branch selected
opencc-LICENSE.txt              OpenCC 1.1.9, Apache-2.0
rapidjson-1.1.0-license.txt     RapidJSON upstream v1.1.0 composite licence
boost-LICENSE_1_0.txt           Boost 1.89.0, BSL-1.0
utfcpp-utf8.h                  utfcpp header including its BSL-1.0 grant
darts-clone-COPYING.txt         darts-clone, BSD-3-Clause
x11-keysym-NOTICE.txt           complete keysym.h, including both notices
x11-keysymdef-NOTICE.txt        complete keysymdef.h, including both notices
alt12dicts-README.txt           Alt12Dicts public-domain statement and scope
alt12dicts-README-orig.txt      author acknowledgment request and AGID exception

RapidJSON's JSON-licensed bin/jsonchecker directory is not compiled or shipped.
Its msinttypes headers are excluded by the current MSVC compiler conditional;
the complete upstream licence file is retained without implying they are used.
TCLAP is used only by OpenCC command-line tools, which are not shipped here.
The unused bundled marisa-0.2.6, tests, benchmarks, Python bindings, and upstream
word lists and OpenCC dictionaries are outside this package's payload.

The Alt12Dicts acknowledgment is also in NOTICE.txt and the generated provenance.
EpoCanva's own code and build/control scripts use AGPL-3.0-only; the complete GNU
text is at ../LICENSE. ../license-decision.json records the user's internal unsigned
acceptance scope. No public release or word-list jurisdiction approval is inferred.
