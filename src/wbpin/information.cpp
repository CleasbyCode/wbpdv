void displayInfo() {
	std::cout << R"(

WEBP Data Vehicle (wbpin v1.1). 
Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024.

wbpin is a data-hiding / steganography-like CLI tool for concealing any file type within a WEBP image. 

Compile & run wbpin (Linux):
		
$ g++ main.cpp -O2 -lz -lwebp -s -o wbpin
$ sudo cp wbpin /usr/bin
$ wbpin
		
Usage: wbpin <cover_image> <data_file>
       wbpin --info
		
Share your "file-embedded" WEBP image on compatible sites, such as Mastodon & Tumblr.

Max file size is 9MB. (Image file + Data file).

)";
}