# wpdv 

(***Work in progress. Usable, but probably buggy***).

Use CLI tools ***wdvin*** & ***wdvout*** with a WEBP image, to embed or extract any file, up to **9MB** (cover image + data file).  

Share your "file-embedded" WEBP image on compatible sites, such as ***Mastodon*** & ***Tumblr***.

![Demo Image](https://github.com/CleasbyCode/wpdv/blob/main/demo_image/wpdv_99489.webp)  
***Image credit:*** [***𝑮𝒐𝒐𝒅 𝑪𝒉𝒐𝒐𝒔𝒆𝒏𝒐𝒐𝒏 (@_o__o_l)***](https://x.com/_o__o_l) / ***PIN: 3573503118***

Your embedded file is ***compressed*** and ***encrypted*** with ***PIN*** protection.  

## Usage (Linux - wdvin / wdvout)

```console

user1@linuxbox:~/Downloads/wpdv-main/src/wdvin$ g++ main.cpp -O2 -lz -lwebp -s -o wdvin
user1@linuxbox:~/Downloads/wpdv-main/src/wdvin$ sudo cp wdvin /usr/bin

user1@linuxbox:~/Desktop$ wdvin 

Usage: wdvin <cover_image> <data_file>  
       wdvin --info

user1@linuxbox:~/Desktop$ wdvin Cover_Image.webp Hidden_File.zip
  
Saved "file-embedded" WEBP image: wpdv_12462.webp (143029 bytes).

Recovery PIN: [***2166776980***]

Important: Please remember to keep your PIN safe, so that you can extract the hidden file.

Complete!

user1@linuxbox:~/Downloads/wpdv-main/src/wdvout$ g++ main.cpp -O2 -lz -s -o wdvout
user1@linuxbox:~/Downloads/wdpv-main/src/wdvout$ sudo cp wdvout /usr/bin

user1@linuxbox:~/Desktop$ wdvout

Usage: wdvout <file_embedded_image>
       wdvout --info
        
user1@linuxbox:~/Desktop$ wdvout wpdv_12462.webp

PIN: **********

Extracted hidden file: Hidden_File.zip (6165 bytes).

Complete! Please check your file.
```
https://github.com/user-attachments/assets/76e74a80-e16a-489e-b066-24b0f747cc92  

https://github.com/user-attachments/assets/fc18fc85-872c-44bd-881e-b2ff83a602c3  

You can try [***wpdv Web App***](https://cleasbycode.co.uk/wpdv/index/) if you don't want to download and compile the source code.  

https://github.com/user-attachments/assets/7edab77e-14ed-4ddc-b1b6-8a4f74fd9d40

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##


