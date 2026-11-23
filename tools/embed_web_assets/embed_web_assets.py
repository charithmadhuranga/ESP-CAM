#!/usr/bin/env python3
"""
VisionCam Web UI Asset Embedder

Embeds HTML, CSS, and JavaScript into the ESP32 firmware as gzip-compressed
C arrays. Generates:
  - firmware/src/web_assets/web_asset_N.c (gzip byte arrays)
  - firmware/src/web_assets/web_assets_manifest.h (manifest header)
  - firmware/src/web_assets/web_assets_manifest.c (manifest data)
"""

import gzip
import hashlib
import os
import shutil
import sys
from pathlib import Path


MIME_TYPES = {
    '.html': 'text/html',
    '.css': 'text/css',
    '.js': 'text/javascript',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.json': 'application/json',
    '.txt': 'text/plain',
}


def find_assets(source_dir):
    """Find all HTML, CSS, JS, SVG files in the source directory."""
    assets = []
    for f in Path(source_dir).rglob('*'):
        if f.is_file() and f.suffix.lower() in MIME_TYPES:
            assets.append(f)
    return sorted(assets)


def gzip_data(data: bytes) -> bytes:
    """Gzip-compress raw bytes."""
    return gzip.compress(data, compresslevel=9)


def sha256(data: bytes) -> str:
    """Compute SHA-256 hex digest."""
    return hashlib.sha256(data).hexdigest()


def asset_to_c_var(name: str) -> str:
    """Convert an asset path/name to a C variable name."""
    safe = name.replace('.', '_').replace('/', '_').replace('-', '_')
    return f'web_asset_{safe}'


def generate_asset_c_file(var_name: str, data: bytes, raw_size: int,
                          comp_size: int, sha_hash: str, url: str) -> str:
    """Generate C source file content for a gzip-compressed asset."""
    lines = []
    lines.append(f'// Asset: {url}')
    lines.append(f'// Raw size: {raw_size} bytes')
    lines.append(f'// Compressed size: {comp_size} bytes')
    lines.append(f'// SHA-256: {sha_hash}')
    lines.append('')
    lines.append('#include <stdint.h>')
    lines.append('')
    lines.append(f'const uint8_t {var_name}[] = {{')
    
    bytes_per_line = 16
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i:i + bytes_per_line]
        hex_values = ', '.join(f'0x{b:02x}' for b in chunk)
        lines.append(f'    {hex_values},')
    
    lines.append('};')
    lines.append('')
    return '\n'.join(lines)


def embed_assets(source_dir, target_dir):
    """Copy and embed all web assets into the target directory."""
    print(f"Scanning for assets in: {source_dir}")

    assets = find_assets(source_dir)
    if not assets:
        print("No web assets found!")
        return 1

    total_files = 0
    total_raw = 0
    total_comp = 0

    manifest_entries = []

    # Cache-busting: every asset URL referenced from an HTML page gets
    # "?v=<first 12 hex of its SHA-256>". The page itself is served
    # no-cache, so after a firmware update browsers see new URLs and can
    # never keep running a stale app.js (they previously could, for up to
    # a year, because assets were cached max-age=31536000 under fixed URLs).
    # The firmware ignores the query string when looking an asset up.
    asset_versions = {}
    for asset_path in assets:
        if asset_path.suffix.lower() == '.html':
            continue
        rel = '/' + str(asset_path.relative_to(source_dir)).replace('\\', '/')
        with open(asset_path, 'rb') as f:
            asset_versions[rel] = sha256(f.read())[:12]

    def versioned_html(html: bytes) -> bytes:
        text = html.decode('utf-8')
        for rel, ver in asset_versions.items():
            for attr in ('src', 'href'):
                text = text.replace(f'{attr}="{rel}"', f'{attr}="{rel}?v={ver}"')
        return text.encode('utf-8')

    for asset_path in assets:
        # Read raw file
        with open(asset_path, 'rb') as f:
            raw_data = f.read()
        if asset_path.suffix.lower() == '.html':
            raw_data = versioned_html(raw_data)
        
        raw_size = len(raw_data)
        
        # Compute URL from path relative to source_dir
        rel_path = str(asset_path.relative_to(source_dir))
        url = '/' + rel_path.replace('\\', '/')
        
        mime = MIME_TYPES.get(asset_path.suffix.lower(), 'application/octet-stream')
        sha_hash = sha256(raw_data)
        
        # Gzip compress
        comp_data = gzip_data(raw_data)
        comp_size = len(comp_data)
        
        print(f"  {url}: raw={raw_size:,} comp={comp_size:,} ({comp_size/raw_size*100:.0f}%) mime={mime}")
        
        total_raw += raw_size
        total_comp += comp_size
        total_files += 1
        
        # Generate C variable name
        var_name = asset_to_c_var(url)
        
        # Generate C file content
        c_content = generate_asset_c_file(var_name, comp_data, raw_size, comp_size,
                                  sha_hash, url)
        
        # Write C file
        c_path = Path(target_dir) / f'{var_name}.c'
        with open(c_path, 'w') as f:
            f.write(c_content)
        
        manifest_entries.append({
            'url': url,
            'var_name': var_name,
            'raw_size': raw_size,
            'comp_size': comp_size,
            'hash': sha_hash,
            'mime': mime,
            'c_path': c_path,
        })

    # Generate manifest header
    header_lines = []
    header_lines.append('// Auto-generated by tools/embed_web_assets.py — DO NOT EDIT')
    header_lines.append('')
    header_lines.append('#ifndef WEB_ASSETS_MANIFEST_H')
    header_lines.append('#define WEB_ASSETS_MANIFEST_H')
    header_lines.append('')
    header_lines.append('#include <stdint.h>')
    header_lines.append('#include <stddef.h>')
    header_lines.append('')
    header_lines.append('typedef struct {')
    header_lines.append('    const char *url;')
    header_lines.append('    const uint8_t *data;')
    header_lines.append('    uint32_t raw_size;')
    header_lines.append('    uint32_t comp_size;')
    header_lines.append('    const char *hash;')
    header_lines.append('    const char *mime;')
    header_lines.append('} WebAsset;')
    header_lines.append('')
    header_lines.append(f'extern const WebAsset web_assets[{len(manifest_entries)}];')
    header_lines.append(f'extern const size_t web_assets_count;')
    header_lines.append('')
    header_lines.append('#endif')
    
    header_path = Path(target_dir) / 'web_assets_manifest.h'
    with open(header_path, 'w') as f:
        f.write('\n'.join(header_lines) + '\n')

    # Generate manifest C file
    manifest_lines = []
    manifest_lines.append('// Auto-generated by tools/embed_web_assets.py — DO NOT EDIT')
    manifest_lines.append('')
    manifest_lines.append('#include "web_assets_manifest.h"')
    manifest_lines.append('')
    
    # External declarations
    for entry in manifest_entries:
        manifest_lines.append(f'extern const uint8_t {entry["var_name"]}[];')
    
    manifest_lines.append('')
    manifest_lines.append('const WebAsset web_assets[] = {')
    for i, entry in enumerate(manifest_entries):
        comma = ',' if i < len(manifest_entries) - 1 else ''
        manifest_lines.append('    {')
        manifest_lines.append(f'        .url = "{entry["url"]}",')
        manifest_lines.append(f'        .data = {entry["var_name"]},')
        manifest_lines.append(f'        .raw_size = {entry["raw_size"]}u,')
        manifest_lines.append(f'        .comp_size = {entry["comp_size"]}u,')
        manifest_lines.append(f'        .hash = "{entry["hash"]}",')
        manifest_lines.append(f'        .mime = "{entry["mime"]}",')
        manifest_lines.append(f'    }}{comma}')
    manifest_lines.append('};')
    manifest_lines.append(f'const size_t web_assets_count = {len(manifest_entries)}u;')
    
    manifest_path = Path(target_dir) / 'web_assets_manifest.c'
    with open(manifest_path, 'w') as f:
        f.write('\n'.join(manifest_lines) + '\n')

    # Print summary
    print(f"\n{'=' * 60}")
    print(f"Total files: {total_files}")
    print(f"Total raw size:  {total_raw:,} bytes ({total_raw / 1024:.1f} KB)")
    print(f"Total comp size: {total_comp:,} bytes ({total_comp / 1024:.1f} KB)")
    print(f"Compression: {(1 - total_comp / total_raw) * 100:.0f}% smaller")
    print(f"Assets embedded to: {target_dir}")
    print(f"{'=' * 60}")

    return 0


def main():
    if len(sys.argv) < 3:
        print("Usage: embed_web_assets.py <source-dir> <target-dir>")
        return 1

    source_dir = sys.argv[1]
    target_dir = sys.argv[2]

    if not os.path.exists(source_dir):
        print(f"Error: Source directory '{source_dir}' does not exist")
        return 1

    os.makedirs(target_dir, exist_ok=True)

    print(f"Embedding web assets from '{source_dir}' to '{target_dir}'")
    print()

    return embed_assets(source_dir, target_dir)


if __name__ == '__main__':
    sys.exit(main())