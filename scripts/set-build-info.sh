#!/usr/bin/env bash

set -euo pipefail

path=""
version="localdev"
build_date="localdev"
build_platform="localdev"
build_architecture="localdev"
git_commit="localdev"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --path)
      path="$2"
      shift 2
      ;;
    --version)
      version="$2"
      shift 2
      ;;
    --build-date)
      build_date="$2"
      shift 2
      ;;
    --build-platform)
      build_platform="$2"
      shift 2
      ;;
    --build-architecture)
      build_architecture="$2"
      shift 2
      ;;
    --git-commit)
      git_commit="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$path" ]]; then
  echo "Missing required argument: --path" >&2
  exit 1
fi

escape_cpp_string() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

version_escaped="$(escape_cpp_string "$version")"
build_date_escaped="$(escape_cpp_string "$build_date")"
build_platform_escaped="$(escape_cpp_string "$build_platform")"
build_architecture_escaped="$(escape_cpp_string "$build_architecture")"
git_commit_escaped="$(escape_cpp_string "$git_commit")"

tmp_file="$(mktemp)"

awk \
  -v releaseVersion="$version_escaped" \
  -v buildDate="$build_date_escaped" \
  -v buildPlatform="$build_platform_escaped" \
  -v buildArchitecture="$build_architecture_escaped" \
  -v gitCommit="$git_commit_escaped" \
  '
  /static constexpr auto releaseVersion = / {
    print "    static constexpr auto releaseVersion = \"" releaseVersion "\";";
    next;
  }
  /static constexpr auto buildDate = / {
    print "    static constexpr auto buildDate = \"" buildDate "\";";
    next;
  }
  /static constexpr auto buildPlatform = / {
    print "    static constexpr auto buildPlatform = \"" buildPlatform "\";";
    next;
  }
  /static constexpr auto buildArchitecture = / {
    print "    static constexpr auto buildArchitecture = \"" buildArchitecture "\";";
    next;
  }
  /static constexpr auto gitCommit = / {
    print "    static constexpr auto gitCommit = \"" gitCommit "\";";
    next;
  }
  {
    print;
  }
  ' "$path" > "$tmp_file"

mv "$tmp_file" "$path"
