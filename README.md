# wbpdv 

***wbpdv*** is a fast, easy-to-use steganography command-line tool for concealing any file type via a **WebP** image.  

There is also a [***Web edition***](https://cleasbycode.co.uk/wbpdv/app/), which you can use immediately, as a convenient alternative to downloading and compiling the CLI source code.

![Demo Image](https://github.com/CleasbyCode/wbpdv/blob/main/demo_image/wbpdv_180552.webp)  
***Image credit:*** [***@blackowl777***](https://x.com/blackowl777) / ***PIN: 7438463291507255314***

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***wbpdv*** embeds files within ***Chunks*** of a ***WebP*** image, such as ***ICCP***, ***EXIF*** & ***XMP***. 

You can conceal any file type up to ***1GB***, although compatible sites (*listed below*) have their own ***much smaller*** size limits and *other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***libdeflate/zlib*** — unless it's already a compressed file type — and encrypted with ***XChaCha20-Poly1305*** using the ***libsodium*** cryptographic library.

## Compilation & Usage (Linux)

```console
$ sudo apt-get update
$ sudo apt install g++ cmake ninja-build util-linux libsodium-dev zlib1g-dev libdeflate-dev libwebp-dev

$ chmod +x compile_wbpdv.sh
$ ./compile_wbpdv.sh

$ sudo cp wbpdv /usr/bin
$ wbpdv

Usage: wbpdv conceal [-b] <cover_image> <secret_file>
       wbpdv recover <cover_image>  
       wbpdv --info

$ wbpdv conceal your_cover_image.webp your_secret_file.doc
   
Saved "file-embedded" WebP image: wbpdv_129462.webp (143029 bytes).

Recovery PIN: [***2166776980318349924***]

Important: Keep your PIN safe, so that you can extract the hidden file.

Complete!
        
$ wbpdv recover wbpdv_129462.webp

PIN: *******************

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```
## Compatible Platforms
*Posting size limit measured by the combined size of the cover image + compressed data file:*  

● ***Mastodon*** (**16MB**)   
● ***Tumblr*** (**9MB**)   
● ***Bluesky*** (**~1MB** | ***-b option***)

wbpdv ***mode*** arguments:
 
  ***conceal*** - Compresses, encrypts and embeds your secret data file within a ***WebP*** cover image.  
  ***recover*** - Decrypts, uncompresses and extracts the concealed data file from a ***WebP*** cover image.
 
wbpdv ***conceal*** mode ***platform*** option:
 
  "***-b***" To create compatible "*file-embedded*" ***WEBP*** images for posting on the ***Bluesky*** platform, you must use the ***-b*** option with ***conceal*** mode.
  ```console
  $ wbpdv conceal -b my_image.webp hidden.doc
  ```
  These images are only compatible for posting on ***Bluesky***. Your embedded data file will be removed if posted on a different platform.
 
  You are also required to use the Python script ***"bsky_post.py"*** (*found in the repo ***src*** folder*) to post the image to ***Bluesky***.
  It will not work if you post images to ***Bluesky*** via the browser site or mobile app.  

  To use the script, you will need to create an [***app password***](https://bsky.app/settings/app-passwords) from your ***Bluesky*** account.  

  Standard ***bsky script*** example of an image post to your bsky profile:

  ```console
  $ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx --image your_image.webp --alt-text "alt-text here (optional)" "standard post text here (required)"
  ```
https://github.com/user-attachments/assets/7c725045-b3f0-4bd1-890f-7810a721ca5c  

https://github.com/user-attachments/assets/5520d223-e179-4ab1-81ee-bfd8a42197f2

https://github.com/user-attachments/assets/27a12ef6-de2d-4c7b-9bff-e85f2de18495

https://github.com/user-attachments/assets/ef39f5fc-caed-4bc7-a80d-ff478095415e

https://github.com/user-attachments/assets/39e89290-ac20-40b6-8a27-289f8680a3d2

## Third-Party Software and Assets

  ### Core application

  - [libsodium](https://github.com/jedisct1/libsodium) — Cryptographic random generation, password hashing, authenticated encryption, secure
    memory clearing, and Base64 decoding.
    License: [ISC](https://github.com/jedisct1/libsodium/blob/master/LICENSE)
    
    Copyright (c) 2013–2026 Frank Denis.

  - [zlib](https://github.com/madler/zlib) — Streaming zlib compression and decompression.
    License: [zlib License](https://github.com/madler/zlib/blob/develop/LICENSE)
    
    Copyright (C) 1995–2026 Jean-loup Gailly and Mark Adler.

  - [libdeflate](https://github.com/ebiggers/libdeflate) — Fast whole-buffer zlib-format compression.
    License: [MIT](https://github.com/ebiggers/libdeflate/blob/master/COPYING)
    
    Copyright 2016 Eric Biggers.
    
    Copyright 2024 Google LLC.

  - [libwebp](https://github.com/webmproject/libwebp) — WebP validation, decoding, and encoding.
    License: [BSD 3-Clause](https://github.com/webmproject/libwebp/blob/main/COPYING), with an
    additional [patent grant](https://github.com/webmproject/libwebp/blob/main/PATENTS).
    
    Copyright (c) 2010, Google Inc. All rights reserved.

  ### Incorporated source and assets

  - [base64simd](https://github.com/WojciechMula/base64simd) — The AVX2 Base64 encoder is adapted from Wojciech Muła’s vector Base64 implementation.
    License: [BSD 2-Clause](https://github.com/WojciechMula/base64simd/blob/master/LICENSE)
    
    Copyright (c) 2015–2018, Wojciech Muła. All rights reserved.

  - [Compact ICC Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles) — [A modified sRGB-v2-micro.icc](https://github.com/saucecontrol/Compact-ICC-Profiles/blob/master/profiles/sRGB-v2-micro.icc)
    profile is embedded in the standard WebP template.
    License: [CC0 1.0 Universal](https://github.com/saucecontrol/Compact-ICC-Profiles/blob/master/license).

  ### Optional Bluesky posting helper

  - Bryan Newbold / ATProto Hacker Cookbook — create_bsky_post.py — Basis for the forked Bluesky
    posting helper (src/bsky/bsky_post.py). 
    For reference see the [Cookbook copy](https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py)
    License: [CC0 1.0 Universal](https://github.com/bluesky-social/cookbook/blob/main/LICENSE-CC0).

  - Requests — HTTP and Bluesky API requests.
    License: [Apache 2.0](https://github.com/psf/requests/blob/main/LICENSE)
    [NOTICE](https://github.com/psf/requests/blob/main/NOTICE)
    
    Copyright 2019 Kenneth Reitz.

  - Beautiful Soup 4 — HTML and Open Graph metadata parsing.
    License: [MIT](https://pypi.org/project/beautifulsoup4/)
    
    Copyright (c) Leonard Richardson.

  - Pillow — Image validation, dimensions, and aspect-ratio handling.
    License: [MIT-CMU](https://github.com/python-pillow/Pillow/blob/main/LICENSE)
    
    PIL copyright © 1997–2011 Secret Labs AB and © 1995–2011 Fredrik Lundh and contributors.
    
    Pillow copyright © 2010 Jeffrey “Alex” Clark and contributors.
    
##


