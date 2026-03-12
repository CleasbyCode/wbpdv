# wbpdv 

***wbpdv*** is a fast, easy-to-use steganography command-line tool for concealing any file type via a **WEBP** image.  

There is also a [***Web edition***](https://cleasbycode.co.uk/wbpdv/app/), which you can use immediately, as a convenient alternative to downloading and compiling the CLI source code.

![Demo Image](https://github.com/CleasbyCode/wbpdv/blob/main/demo_image/wbpdv_180552.webp)  
***Image credit:*** [***@blackowl777***](https://x.com/blackowl777) / ***PIN: 7438463291507255314***

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***wbpdv*** embeds files within ***Chunks*** of a ***WEBP*** image, such as ***ICCP***, ***EXIF*** & ***XMP***. 

You can conceal any file type up to ***1GB***, although compatible sites (*listed below*) have their own ***much smaller*** size limits and *other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***zlib*** and encrypted using the ***libsodium*** cryptographic library.  

## Usage (Linux)

```console
Note: Compiler support for C++23 required.

$ sudo apt install libsodium-dev zlib1g-dev libwebp-dev
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
## Compatible Platforms
*Posting size limit measured by the combined size of the cover image + compressed data file:*  

● ***Mastodon*** (**16MB**)   
● ***Tumblr*** (**9MB**)   
● ***Bluesky*** (**~1MB** | ***-b option***)

wbpdv ***mode*** arguments:
 
  ***conceal*** - Compresses, encrypts and embeds your secret data file within a ***WEBP*** cover image.  
  ***recover*** - Decrypts, uncompresses and extracts the concealed data file from a ***WEBP*** cover image.
 
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

https://github.com/user-attachments/assets/764b043f-0871-41e9-b46b-2e39ce04ed26

https://github.com/user-attachments/assets/ef39f5fc-caed-4bc7-a80d-ff478095415e

## Third-Party Libraries

This project makes use of the following third-party libraries:

- **libsodium**: For cryptographic functions.
  - [**LICENSE**](https://github.com/jedisct1/libsodium/blob/master/LICENSE)
  - Copyright (C) 2013-2025 Frank Denis (github@pureftpd.org)
- **zlib**: General-purpose compression library
  - License: zlib/libpng license (see [***LICENSE***](https://github.com/madler/zlib/blob/develop/LICENSE) file)
  - Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
- **WebP**: Image processing library, developed by Google.
   - **Copyright**: Copyright 2010 Google Inc.
   - **License**: BSD 3-Clause License (see [***LICENSE***](https://github.com/webmproject/libwebp?tab=BSD-3-Clause-1-ov-file#readme) file for details)    
##


