# EpoCanva P2.4 -- assert the generated pilot output against the pinned manifest.
#
# Invoked as:  cmake -DMANIFEST=<lock.json> -DOUT_DIR=<dir> -P this-file
#
# WHY THIS EXISTS. The engine-side pilot case proves the translator behaves on a big
# list. It cannot prove the list on disk IS the list the manifest pins: a truncated
# download, a hand edit, or a converter that stopped counting would all still load
# and still complete. This is the check that does, and it fails on:
#   - lexicon bytes that are not the pinned SHA-256;
#   - a line count that disagrees with the row count in either record;
#   - a conversion report whose own arithmetic does not close;
#   - a manifest edited into describing something other than the file present;
#   - a generated list with no delimited rows in it, which would silently stop the
#     pilot testing the P2.3 word-form work at all.

cmake_minimum_required(VERSION 3.19)

foreach(_required MANIFEST OUT_DIR)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "assert_pilot_output: -D${_required} is required")
  endif()
endforeach()
if(NOT EXISTS "${MANIFEST}")
  message(FATAL_ERROR "assert_pilot_output: manifest not found: ${MANIFEST}")
endif()
file(READ "${MANIFEST}" _manifest)

string(JSON _pin_commit GET "${_manifest}" pinned_commit)
string(JSON _lex_name   GET "${_manifest}" output lexicon_file)
string(JSON _rep_name   GET "${_manifest}" output report_file)
string(JSON _want_sha   GET "${_manifest}" output sha256)
string(JSON _want_rows  GET "${_manifest}" output rows)
string(JSON _want_lc    GET "${_manifest}" output line_count)
string(JSON _want_bytes GET "${_manifest}" output size_bytes)
string(JSON _src_rel    GET "${_manifest}" inputs 0 repo_path)
string(JSON _src_sha    GET "${_manifest}" inputs 0 sha256)

set(_lex "${OUT_DIR}/${_lex_name}")
set(_rep "${OUT_DIR}/${_rep_name}")
if(NOT EXISTS "${_lex}" OR NOT EXISTS "${_rep}")
  message(FATAL_ERROR
    "assert_pilot_output: missing ${_lex} or ${_rep}.\n"
    "Generate them with 'python tools/data/build_english_pilot.py --fetch'\n"
    "(or --source-root <checkout>). This test does not skip: a pilot configure that\n"
    "names data it cannot find is a failure, not an absence of evidence.")
endif()

set(_problems "")
function(_note text)
  string(APPEND _problems "${text}\n")
  set(_problems "${_problems}" PARENT_SCOPE)
endfunction()

macro(_expect label got want)
  if(NOT "${${got}}" STREQUAL "${${want}}")
    message(STATUS "FAIL ${label}: got '${${got}}' want '${${want}}'")
    _note("  ${label}: got '${${got}}' want '${${want}}'")
  else()
    message(STATUS "ok   ${label} = ${${want}}")
  endif()
endmacro()

# --- the file is the pinned file -------------------------------------------------
file(SHA256 "${_lex}" _got_sha)
set(_want_sha_local "${_want_sha}")
set(_got_sha_local "${_got_sha}")
_expect("lexicon sha256 matches the manifest" _got_sha_local _want_sha_local)

file(SIZE "${_lex}" _lex_size)
set(_lex_size_local "${_lex_size}")
_expect("lexicon size matches the manifest" _lex_size_local _want_bytes)

file(READ "${_lex}" _lex_bytes)
if("${_lex_bytes}" MATCHES "\r")
  _note("the generated lexicon contains a CR byte; the contract is LF-only")
else()
  message(STATUS "ok   LF-only output")
endif()

# one record per line: the newline count is the row count, no trailing blank line
string(REGEX REPLACE "[^\n]" "" _only_nl "${_lex_bytes}")
string(LENGTH "${_only_nl}" _newlines)
set(_newlines_local "${_newlines}")
set(_rows_local "${_want_rows}")
_expect("line count equals the manifest row count" _newlines_local _rows_local)

# --- the report closes on itself -------------------------------------------------
file(READ "${_rep}" _report)
string(JSON _rep_rows GET "${_report}" "rows")
set(_a "${_rep_rows}")
set(_b "${_want_rows}")
_expect("report row count equals the manifest" _a _b)

string(JSON _src_rows GET "${_report}" "exclusions" "source_rows")
string(JSON _acc      GET "${_report}" "exclusions" "accepted_rows")
string(JSON _exc      GET "${_report}" "exclusions" "excluded_rows")
math(EXPR _closure "${_acc} + ${_exc}")
set(_c "${_closure}")
set(_d "${_src_rows}")
_expect("accepted + excluded equals source rows" _c _d)

set(_reasons non-ascii-byte word-too-long contains-space
             digit-or-other-punctuation capitalised-letter
             leading-or-trailing-delimiter more-than-one-delimiter duplicate-word)
set(_reason_sum 0)
foreach(_r IN LISTS _reasons)
  string(JSON _v GET "${_report}" "exclusions" "exclusions_by_reason" "${_r}")
  math(EXPR _reason_sum "${_reason_sum} + ${_v}")
endforeach()
set(_e "${_reason_sum}")
set(_f "${_exc}")
_expect("the eight exclusion reasons sum to excluded rows" _e _f)

set(_form_sum 0)
foreach(_form plain-letter-run with-apostrophe with-hyphen)
  string(JSON _v GET "${_report}" "exclusions" "accepted_forms" "${_form}")
  math(EXPR _form_sum "${_form_sum} + ${_v}")
endforeach()
set(_g "${_form_sum}")
set(_h "${_acc}")
_expect("the three accepted forms sum to accepted rows" _g _h)

string(JSON _freq_type TYPE "${_report}" "exclusions" "weights_are_corpus_frequency")
if(NOT _freq_type STREQUAL "BOOLEAN")
  _note("report field weights_are_corpus_frequency is a ${_freq_type}, not a boolean")
else()
  # CMake renders a JSON boolean as ON/OFF, so the check uses CMake's own truth
  # value instead of comparing against the string "false" -- which is exactly what
  # this assertion got wrong on its first run.
  string(JSON _is_freq GET "${_report}" "exclusions" "weights_are_corpus_frequency")
  if(_is_freq)
    _note("report claims the weights ARE corpus frequency; P2.4 forbids that claim")
  else()
    message(STATUS "ok   report records that weights are not corpus frequency")
  endif()
endif()

# --- the pilot actually exercises the P2.3 word forms ----------------------------
string(JSON _n_apostrophe GET "${_report}" "exclusions" "accepted_forms" "with-apostrophe")
string(JSON _n_hyphen     GET "${_report}" "exclusions" "accepted_forms" "with-hyphen")
if(_n_apostrophe LESS 1)
  _note("no apostrophe row survived; the pilot would not test don't at scale")
else()
  message(STATUS "ok   ${_n_apostrophe} apostrophe rows in the pilot list")
endif()
if(_n_hyphen LESS 1)
  _note("no hyphen row survived; the pilot would not test co-operate at scale")
else()
  message(STATUS "ok   ${_n_hyphen} hyphen rows in the pilot list")
endif()

# --- the manifest still describes a real identity --------------------------------
# CMake's regex engine has no {n} repetition operator, so "exactly N hex digits" is
# a length check PLUS a character-class check: the class alone would accept a
# 39-digit prefix. (The first version of this file wrote "^[0-9a-f]{40}$", CMake
# read the braces as literals and matched nothing -- that is how it was found.)
macro(_expect_hex _var _len _label)
  set(_xh_value "${${_var}}")
  string(LENGTH "${_xh_value}" _xh_len)
  if(NOT _xh_len EQUAL ${_len})
    _note("${_label}: exactly ${_len} hex digits expected, got ${_xh_len}: '${_xh_value}'")
  elseif(NOT "${_xh_value}" MATCHES "^[0-9a-f]+$")
    _note("${_label}: not all hex characters: '${_xh_value}'")
  else()
    message(STATUS "ok   ${_label} = ${_xh_value}")
  endif()
  unset(_xh_value)
  unset(_xh_len)
endmacro()

_expect_hex(_pin_commit 40 "manifest pinned_commit")
_expect_hex(_src_sha 64 "manifest input digest for ${_src_rel}")
string(JSON _src_crlf GET "${_manifest}" inputs 0 crlf_pairs)
message(STATUS "ok   manifest records ${_src_crlf} CRLF pairs for the source")

if(_problems)
  message(FATAL_ERROR "PILOT OUTPUT FAIL\n${_problems}")
endif()
message(STATUS "PILOT OUTPUT PASS")
