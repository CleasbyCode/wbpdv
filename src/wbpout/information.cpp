void displayInfo() {
	std::cout << R"(

WEBP Data Vehicle (wbpout v1.1). 
Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024.

wdvout is a CLI tool for extracting hidden data from a wbpin "file-embedded" WEBP image. 

Compile & run wbpout (Linux):
		
$ g++ main.cpp -O2 -lz -s -o wbpout
$ sudo cp wbpout /usr/bin
$ wbpout

Usage: wbpout <file-embedded-image>
       wbpout --info

)";
}