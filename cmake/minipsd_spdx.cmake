# ---------------------------------------------------------------------------
# An SPDX 2.3 fragment describing one package and its files, for a consumer to merge into its own
# SBOM: minipsd's (CMakeLists.txt) and the released plugin zip's (tools/package_vips.cmake).
# Every file is listed with its checksums; SPDX requires SHA-1 per file and derives the package
# verification code from those (sorted, concatenated, hashed again). The timestamp honours
# SOURCE_DATE_EPOCH. Works in a project and in script mode (cmake -P).
#
#   minipsd_spdx(OUTPUT <file> PACKAGE <name> VERSION <x.y.z> HOMEPAGE <url> DOWNLOAD <location>
#                SUMMARY <text> COMMENT <text> BASE <dir> [PREFIX <path/>] FILES <relative paths>)
#
# Each file is read from BASE/<path> and named ./PREFIX<path> in the document.
# ---------------------------------------------------------------------------
set(_minipsd_spdx_template "${CMAKE_CURRENT_LIST_DIR}/minipsd.spdx.json.in")

function(minipsd_spdx)
    cmake_parse_arguments(PARSE_ARGV 0 S "" "OUTPUT;PACKAGE;VERSION;HOMEPAGE;DOWNLOAD;SUMMARY;COMMENT;BASE;PREFIX" "FILES")
    set(_file [=[

    {
      "SPDXID": "SPDXRef-File-@_id@",
      "fileName": "./@S_PREFIX@@_rel@",
      "checksums": [
        { "algorithm": "SHA1", "checksumValue": "@_sha1@" },
        { "algorithm": "SHA256", "checksumValue": "@_sha256@" }
      ],
      "licenseConcluded": "MIT",
      "licenseInfoInFiles": [ "MIT" ],
      "copyrightText": "Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>"
    }]=])
    set(_contains [=[

    {
      "spdxElementId": "SPDXRef-Package-@S_PACKAGE@",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-File-@_id@"
    }]=])
    set(_files "")
    set(_relations "")
    set(_sha1s "")
    foreach(_rel IN LISTS S_FILES)
        string(REGEX REPLACE "[^A-Za-z0-9.-]" "-" _id "${_rel}")
        file(SHA1   "${S_BASE}/${_rel}" _sha1)
        file(SHA256 "${S_BASE}/${_rel}" _sha256)
        list(APPEND _sha1s "${_sha1}")
        string(CONFIGURE "${_file}" _entry @ONLY)
        list(APPEND _files "${_entry}")
        string(CONFIGURE "${_contains}" _entry @ONLY)
        list(APPEND _relations "${_entry}")
    endforeach()
    list(JOIN _files "," MINIPSD_SBOM_FILES)
    list(JOIN _relations "," MINIPSD_SBOM_CONTAINS)
    list(SORT _sha1s)
    list(JOIN _sha1s "" _sha1s)
    string(SHA1 MINIPSD_SBOM_VERIFICATION "${_sha1s}")
    string(TIMESTAMP MINIPSD_SBOM_CREATED "%Y-%m-%dT%H:%M:%SZ" UTC)
    configure_file("${_minipsd_spdx_template}" "${S_OUTPUT}" @ONLY)
endfunction()
