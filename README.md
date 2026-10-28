# wbpdv 

***wbpdv*** is a fast, easy-to-use steganography command-line tool for concealing any file type via a **WebP** image.  

There is also a [***Web edition***](https://cleasbycode.co.uk/wbpdv/app/), which you can use immediately, as a convenient alternative to downloading and compiling the CLI source code.

![Demo Image](https://github.com/CleasbyCode/wbpdv/blob/main/demo_image/wbpdv_188189.webp)  
***Demo Image -*** ***KEY: YT2m-hZFa-TFqa-3kC9-6amo-qyvh***

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***wbpdv*** embeds files within ***Chunks*** of a ***WebP*** image, such as ***ICCP*** (Mastodon/Tumblr), ***EXIF*** & ***XMP*** (Bluesky). 

You can conceal any file type up to ***1GiB***, although compatible sites (*listed below*) have their own ***much smaller*** size limits and other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***libdeflate/zlib*** (large files that won't compress are stored as-is) and encrypted with ***XChaCha20-Poly1305*** (***libsodium*** secretstream) under a key derived by ***Argon2id*** from a randomly generated ***128-bit recovery KEY***. The KEY is displayed once, at the end of ***conceal***, as 24 letters and digits in groups of four (*XXXX-XXXX-XXXX-XXXX-XXXX-XXXX*), and is never stored anywhere: without it the concealed file cannot be recovered. Its last two characters are a check, so most typing mistakes are reported straight away.

Images created before ***wbpdv 4.0*** used a numeric ***recovery PIN*** instead. They remain recoverable with that PIN (*leading zeros are ignored, as before*): ***recover*** reads the image's version and prompts for a ***KEY*** or ***PIN*** to match.

## Compilation & Usage (Linux)

```console
$ sudo apt update
$ sudo apt install g++ cmake ninja-build util-linux libsodium-dev zlib1g-dev libdeflate-dev libwebp-dev

$ chmod +x compile_wbpdv.sh
$ ./compile_wbpdv.sh

$ sudo cp wbpdv /usr/bin
$ wbpdv

Usage: wbpdv conceal [-b] <cover_image> <secret_file>
       wbpdv recover <cover_image>  
       wbpdv --info

$ wbpdv conceal your_cover_image.webp your_secret_file.doc
   
Saved "file-embedded" WEBP image: wbpdv_129462.webp (143029 bytes).

Recovery KEY: [***1vSe-caX6-dEbg-dhT6-q3vp-dizz***]

Important: Keep your KEY safe, so that you can extract the hidden file.

Complete!
        
$ wbpdv recover wbpdv_129462.webp

KEY: ****-****-****-****-****-****

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```
## Compatible Platforms
*Posting size limit measured by the combined size of the cover image + your compressed hidden data file (payload):*  

● ***Mastodon*** (**16MiB**)   
● ***Tumblr***   (**9MiB**)   
● ***Bluesky***  (**2,000,000 bytes | ~1.9MiB**)

wbpdv ***mode*** arguments:
 
  ***conceal*** - Compresses, encrypts and embeds your secret data file within a ***WebP*** cover image.  
  ***recover*** - Decrypts, uncompresses and extracts the concealed data file from a ***WebP*** cover image (*recovery KEY/PIN required*).

  The ***recover*** prompt says which secret the image takes: `KEY:` for an image made with ***wbpdv 4.0*** or later, `PIN:` for an older one. Error messages follow the same rule.

  The KEY printed by ***conceal*** is the only copy. If it cannot be written out (*for example, output redirected to a full disk or a closed pipe*), ***conceal*** deletes the image it just saved and exits with an error, as that image could never be recovered.

  When the KEY/PIN is typed at a terminal, each character is masked. If ***recover*** cannot turn off the terminal's echo, it refuses to read the KEY/PIN rather than show it on screen. A KEY/PIN piped in from another program is still accepted.
 
wbpdv ***conceal*** mode ***platform*** option:
 
  "***-b***" To create compatible "*file-embedded*" ***WEBP*** images for posting on the ***Bluesky*** platform, you must use the ***-b*** option with ***conceal*** mode.
  ```console
  $ wbpdv conceal -b my_image.webp hidden.doc
  ```
  These images are only compatible for posting on ***Bluesky***. Your embedded data file will be removed if posted on a different platform.
 
  You are also required to use the Python script [create_bsky_post.py](https://github.com/CleasbyCode/wbpdv/blob/main/src/bsky/create_bsky_post.py) (found in the repo ***src/bsky*** folder) to post the image to ***Bluesky***.
  It will not work if you post images to ***Bluesky*** via the browser site or mobile app.  

  To use the script, you will need to create an [***app password***](https://bsky.app/settings/app-passwords) from your ***Bluesky*** account.  

  See the [create_bsky_post.py](https://github.com/CleasbyCode/wbpdv/blob/main/src/bsky/create_bsky_post.py) script in the src/bsky folder for some basic usage examples.
  
https://github.com/user-attachments/assets/7c725045-b3f0-4bd1-890f-7810a721ca5c  

https://github.com/user-attachments/assets/5520d223-e179-4ab1-81ee-bfd8a42197f2

https://github.com/user-attachments/assets/27a12ef6-de2d-4c7b-9bff-e85f2de18495

https://github.com/user-attachments/assets/ef39f5fc-caed-4bc7-a80d-ff478095415e

https://github.com/user-attachments/assets/39e89290-ac20-40b6-8a27-289f8680a3d2

## Recovery KEY format

This section is for anyone writing a compatible implementation. Everything below is fixed by wbpdv 4.0.

***The KEY*** is 16 bytes from a cryptographic random number generator (*never all zero*). It is written as:

1. ***Body*** — the 16 bytes read as one 128-bit big-endian integer, written in Base58 as exactly 22 digits, most significant first, padded on the left with `1` (*the zero digit*). Alphabet (*Bitcoin's*): `123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz`.
2. ***Check*** — two more characters:

   ```
   digest = BLAKE2b-256(ASCII("wbpdv key check v1") || key_bytes)   (18-byte label, no NUL; unkeyed)
   value  = big_endian_u32(digest[0..4]) mod 3364
   check  = alphabet[value / 58] || alphabet[value mod 58]
   ```
3. ***Display*** — the 24 characters in six groups of four, joined with `-`.

When a KEY is entered, spaces, tabs and `-` are ignored and letters are case-sensitive. A numeric entry is an old (*pre-4.0*) PIN: leading zeros are ignored, then 1–20 digits remain (*at most 18446744073709551615, never 0*).

| Key bytes (hex) | KEY |
| --- | --- |
| `00000000000000000000000000000001` | `1111-1111-1111-1111-1111-12NH` |
| `00000000000000000000000000000f2c` | `1111-1111-1111-1111-1112-9y11` (*check value 0*) |
| `0000000000000000000000000000057a` | `1111-1111-1111-1111-1111-RBzz` (*check value 3363*) |
| `000102030405060708090a0b0c0d0e0f` | `112d-rXXU-ifSr-RnXL-GbXg-8Egn` |
| `80000000000000000000000000000000` | `GokL-Usho-3eiV-vNYN-d1wg-fyVk` |
| `ffffffffffffffffffffffffffffffff` | `YcVf-xkQb-6JRz-qk5k-F2tN-LvN3` |

***Encryption key*** — Argon2id (*libsodium `crypto_pwhash`, ALG_ARGON2ID13, opslimit 2, memlimit 64 MiB, both recorded in the image*) with the 16 raw key bytes as the password and the random salt stored in the image (*metadata version KDF4, which has KDF3's layout*). Old images (*KDF2 and KDF3*) hash the PIN's decimal digits instead.

## Third-Party Software and Assets

  ### Core application

  - [libsodium](https://github.com/jedisct1/libsodium) — cryptographic random generation, Argon2id
  key derivation and XChaCha20-Poly1305 secret streams. Dynamically linked as a system library.
      
      License: [ISC License](https://github.com/jedisct1/libsodium/blob/master/LICENSE)
    
      Copyright (c) 2013–2026 Frank Denis.

  - [zlib](https://github.com/madler/zlib) — Streaming zlib compression and decompression. Dynamically linked as a system library.

    License: [zlib License](https://github.com/madler/zlib/blob/develop/LICENSE)
    
    Copyright (C) 1995–2026 Jean-loup Gailly and Mark Adler.

  - [libdeflate](https://github.com/ebiggers/libdeflate) — Fast whole-buffer zlib-format compression. Dynamically linked as a system library.

    License: [MIT](https://github.com/ebiggers/libdeflate/blob/master/COPYING)
    
    Copyright 2016 Eric Biggers.
    
    Copyright 2024 Google LLC.

  - [libwebp](https://github.com/webmproject/libwebp) — WebP validation, decoding, and encoding. Dynamically linked as a system library.
    
    License: [BSD 3-Clause](https://github.com/webmproject/libwebp/blob/main/COPYING), with an
    additional [patent grant](https://github.com/webmproject/libwebp/blob/main/PATENTS).
    
    Copyright (c) 2010, Google Inc. All rights reserved.

  ### Incorporated source and assets

  - [Compact ICC Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles) — [A modified sRGB-v2-micro.icc](https://github.com/saucecontrol/Compact-ICC-Profiles/blob/master/profiles/sRGB-v2-micro.icc)
    profile is embedded in the standard WebP template.

    License: [CC0 1.0 Universal](https://github.com/saucecontrol/Compact-ICC-Profiles/blob/master/license).

   ### Optional Bluesky posting helper

  - Bryan Newbold / ATProto Hacker Cookbook — create_bsky_post.py — Basis for the [forked](https://gist.github.com/CleasbyCode/1eb678ca1fa1975b1c1e20aeec33637e) Bluesky posting helper (src/bsky/create_bsky_post.py). 
    For reference see the [Cookbook copy](https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py)

    License: [CC0 1.0 Universal](https://github.com/bluesky-social/cookbook/blob/main/LICENSE-CC0).

  - Requests — HTTP and Bluesky API requests.

    License: [Apache 2.0](https://github.com/psf/requests/blob/main/LICENSE) / [NOTICE](https://github.com/psf/requests/blob/main/NOTICE)
    
    Copyright 2019 Kenneth Reitz.

  - Beautiful Soup 4 — HTML and Open Graph metadata parsing.

    License: [MIT](https://pypi.org/project/beautifulsoup4/)
    
    Copyright (c) Leonard Richardson.

  - Pillow — Image validation, dimensions, and aspect-ratio handling.

    License: [MIT-CMU](https://github.com/python-pillow/Pillow/blob/main/LICENSE)
    
    PIL copyright © 1997–2011 Secret Labs AB and © 1995–2011 Fredrik Lundh and contributors.
    
    Pillow copyright © 2010 Jeffrey “Alex” Clark and contributors.
    
##


