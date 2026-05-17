@echo off
signtool sign /f ./cert.pfx /p Sunmutian123 /fd SHA256 ./x64/Release/MSFSMap.exe