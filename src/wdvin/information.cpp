void displayInfo() {
	std::cout << R"(

WEBP Data Vehicle (wdvin v1.0). 
Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024.

wdvin is a data-hiding / steganography-like CLI tool for concealing any file type within a WEBP image. 

Compile & run wdvin (Linux):
		
$ g++ main.cpp -O2 -lz -lwebp -s -o wdvin
$ sudo cp wdvin /usr/bin
$ wdvin
		
Usage: wdvin <cover_image> <data_file>
       wdvin --info
		
Share your "file-embedded" WEBP image on compatible sites, such as Mastodon & Tumblr.

Max file size is 9MB. (Image file + Data file).

)";
}