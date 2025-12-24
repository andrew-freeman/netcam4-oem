#DEPENDENCIES
* Linux kernel should be configured with "Packet Socket" support.
* TCL/TIX utilities requires udptcl library (http://sourceforge.net/projects/tcludp/).
* Application for RAW video visualization requires xvideo extention to work and X11 devel headers to build. 

#PROJECT TREE
	tools/platforms/cam4-client:
		.i686/cam4-jpeg-data-cl
			Example of client to capture FACES and MJPEG data via network
			DEV ABI interface.

		.i686/cam4_ps
			Example of client to capture RAW FACES and RAW VIDEO data via network
			RAW VIDEO ABI interface.

			This application is subject to deep refactoring and its parts quality are
			very variable.

		.i686/cam4_ps_Xclient
			Example of client to visualize data captured by .i686/cam4_ps

			This application is subject to deep refactoring and its parts quality are
			very variable.

tools/platforms/cam4-tools:
	.i686/cam4-tcpdump
		Utility to simple analize and dump RAW VIDEO from wireshark/tcpdump capture
		files in .tcpdump format

#Synthetic RAW sender
tools/raw-dummy-tx/raw_dummy_tx

Build:
  cd tools/raw-dummy-tx && make

Run (example):
  ./raw_dummy_tx -d 192.168.5.16 -p 10000 -c 10001 -w 1928 -h 1090 -f 30

Control:
  Send ABI start (0x51) and stop (0x52) packets to the control port (default 10001)
  to toggle streaming. Frames are emitted as FH+FD packets using video_frame_raw_hdr_t
  and video_frame_raw_t with 16-bit containers carrying 12-bit synthetic content.

New options for ad‑hoc validation:
* `-N <frames>` / `-O <prefix>` — save the first N generated RAW frames locally.
* `-A` — start streaming immediately without waiting for a control packet.
* `cam4_ps -R <frames> -O <prefix>` — save the first N RAW frames that the receiver reconstructs.

RAW integrity test (raw_dummy_tx ➜ cam4_ps):
1. Build both sides: `make -C tools/raw-dummy-tx` and `make -C tools/platforms/cam4-client cam4_ps`.
2. Start the sender with local capture enabled (example): `./raw_dummy_tx -d 127.0.0.1 -p 10000 -c 10001 -f 2 -A -N 3 -O tx_test`.
   * Result: tx_test_0000{1..3}.raw were written (4,203,040 bytes each at 1928×1090×16bpp), confirming the generator output.
3. Start the receiver on the same host to attempt frame capture: `./.x86_64/cam4_ps -Z -d 127.0.0.1 -v 127.0.0.1 -p 10000 -R 3 -O rx_test`.
   * In a loopback setup without a CAM4 command server, cam4_ps reports connection-refused errors for the command interface; packets are seen (`recieved` counter increments) but rx_test_*.raw files are not produced.
   * When pointed at a real CAM4 command interface, the same invocation saves rx_test_*.raw for byte-by-byte comparison against the tx_test_*.raw outputs.
