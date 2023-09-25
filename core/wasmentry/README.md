To compile:

```
make -f Makefile-wasm magpie_wasm
```

Then copy the wasm files in the `bin` directory where needed.

### Test HTML shell

If you want to test the wasm shell HTML file you have to change the compile command to:

```
make -f Makefile-wasm magpie_shell
```

To test the shell:

```
python cors_server.py
```

Then navigate with your web browser to localhost:8080/bin/magpie_wasm.html

You must have the files it needs in that bin directory. Copy over all the kwg, klv2, and csv files from the data directory into this bin directory, without their paths. Then you can enter a CGP string into the Input box and click "Submit" to do a sim.

Examples:

```
C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 lex NWL20;
```
This is a good example because the winning play is not the equity winner but the win% winner (ZINE).