# wbpdv 

***wbpdv*** is a fast, easy-to-use steganography command-line tool for concealing and extracting any file type via a **WEBP** image.  

There is also a [***Web edition***](https://cleasbycode.co.uk/wbpdv/app/), which you can use immediately, as a convenient alternative to downloading and compiling the CLI source code.

![Demo Image](https://github.com/CleasbyCode/wbpdv/blob/main/demo_image/wbpdv_34225.webp)  
***Image credit:*** [***@TonyKelner***](https://x.com/TonyKelner) / ***PIN: 1439711818***

Your embedded file is ***compressed*** and ***encrypted*** with ***PIN*** protection.  

## Compatible Platforms
*Posting size limit measured by the combined size of the cover image + compressed data file:*  

● ***Mastodon*** & ***Tumblr*** (**9MB**).  

## Usage (Linux)

```console
Note: Compiler support for C++23 required.

$ sudo apt libsodium-dev zlib1g-dev libwebp-dev
$ chmod +x compile_wbpdv.sh
$ ./compile_wbpdv.sh

Compiling wbpdv...
Compilation successful. Executable 'wbpdv' created.

$ sudo cp wbpdv /usr/bin
$ wbpdv

Usage: wbpdv conceal [-b] <cover_image> <secret_file>
       wbpdv recover <cover_image>  
       wbpdv --info

$ wbpdv conceal your_cover_image.webp your_secret_file.doc
   
Saved "file-embedded" WEBP image: wbpdv_129462.webp (143029 bytes).

Recovery PIN: [***2166776980318349924***]

Important: Keep your PIN safe, so that you can extract the hidden file.

Complete!
        
$ wbpdv recover wbpdv_129462.webp

PIN: *******************

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```


https://github.com/user-attachments/assets/795709bb-6b00-4924-9597-61cc27b65bee


https://github.com/user-attachments/assets/078ec4cf-36ab-4c85-a8dd-9b9a41a98889


## Third-Party Libraries

This project makes use of the following third-party libraries:

- **zlib**: General-purpose compression library
  - License: zlib/libpng license (see [***LICENSE***](https://github.com/madler/zlib/blob/develop/LICENSE) file)
  - Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
    
- **WebP**: Image processing library, developed by Google.
   - **Copyright**: Copyright 2010 Google Inc.
   - **License**: BSD 3-Clause License (see [***LICENSE***](https://github.com/webmproject/libwebp?tab=BSD-3-Clause-1-ov-file#readme) file for details)    
##


