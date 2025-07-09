# wbpdv 

A *"steganography-like"* command-line utility consisting of two CLI tools, ***wbpin***, *used for embedding a data file within a ***WEBP*** cover image*, and ***wbpout***, *used for extracting the hidden file from the cover image.*  

*There is also a ***wbpdv Web App***, which you can try [***here***](https://cleasbycode.co.uk/wbpdv/index/) as a convenient alternative to downloading and compiling the CLI source code.*    

![Demo Image](https://github.com/CleasbyCode/wbpdv/blob/main/demo_image/wbpdv_34225.webp)  
***Image credit:*** [***@TonyKelner***](https://x.com/TonyKelner) / ***PIN: 1439711818***

Your embedded file is ***compressed*** and ***encrypted*** with ***PIN*** protection.  

(*You can try the [***wbpdv Web App, here,***](https://cleasbycode.co.uk/wbpdv/index/) if you don't want to download and compile the CLI source code.*)

## Usage (Linux - wbpin / wbpout)

```console

user1@linuxbox:~/Downloads/wbpdv-main/src/wbpin$ g++ main.cpp -O2 -lz -lwebp -s -o wbpin
user1@linuxbox:~/Downloads/wbpdv-main/src/wbpin$ sudo cp wbpin /usr/bin

user1@linuxbox:~/Desktop$ wbpin 

Usage: wbpin <cover_image> <data_file>  
       wbpin --info

user1@linuxbox:~/Desktop$ wbpin Cover_Image.webp Hidden_File.zip
  
Saved "file-embedded" WEBP image: wbpdv_12462.webp (143029 bytes).

Recovery PIN: [***2166776980***]

Important: Please remember to keep your PIN safe, so that you can extract the hidden file.

Complete!

user1@linuxbox:~/Downloads/wbpdv-main/src/wbpout$ g++ main.cpp -O2 -lz -s -o wbpout
user1@linuxbox:~/Downloads/wbpdv-main/src/wbpout$ sudo cp wbpout /usr/bin

user1@linuxbox:~/Desktop$ wbpout

Usage: wbpout <file_embedded_image>
       wbpout --info
        
user1@linuxbox:~/Desktop$ wbpout wbpdv_12462.webp

PIN: **********

Extracted hidden file: Hidden_File.zip (6165 bytes).

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


