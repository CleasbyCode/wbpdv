void displayInfo() {
	std::cout << R"(

WEBP Data Vehicle (wdvout v1.1). 
Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024.

wdvout is a CLI tool for extracting hidden data from a wdvin "file-embedded" WEBP image. 

Compile & run wdvout (Linux):
		
$ g++ main.cpp -O2 -lz -s -o wdvout
$ sudo cp wdvcout /usr/bin
$ wdvout

Usage: wdvout <file-embedded-image>
       wdvout --info

)";
}
