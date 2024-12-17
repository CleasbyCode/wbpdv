# wbpdv 

Use CLI tools ***wbpin*** & ***wbpout*** with a WEBP image, to hide/extract any file, up to **9MB** (cover image + data file).  

Share your "file-embedded" WEBP image on compatible sites, such as ***Mastodon*** & ***Tumblr***.

![Demo Image](https://github.com/CleasbyCode/wbpdv/blob/main/demo_image/wbpdv_97273.webp)  
***Image credit:*** [***𝑮𝒐𝒐𝒅 𝑪𝒉𝒐𝒐𝒔𝒆𝒏𝒐𝒐𝒏 (@_o__o_l)***](https://x.com/_o__o_l) / ***PIN: 1701640012***

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
https://github.com/user-attachments/assets/76e74a80-e16a-489e-b066-24b0f747cc92  

https://github.com/user-attachments/assets/fc18fc85-872c-44bd-881e-b2ff83a602c3  

https://github.com/user-attachments/assets/7edab77e-14ed-4ddc-b1b6-8a4f74fd9d40

## Third-Party Libraries

This project makes use of the following third-party libraries:

- **zlib**: General-purpose compression library
  - License: zlib/libpng license (see [***LICENSE***](https://github.com/madler/zlib/blob/develop/LICENSE) file)
  - Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
    
- **WebP**: Image processing library, developed by Google.
   - **Copyright**: Copyright 2010 Google Inc.
   - **License**: BSD 3-Clause License (see [***LICENSE***](https://github.com/webmproject/libwebp?tab=BSD-3-Clause-1-ov-file#readme) file for details)    
##


