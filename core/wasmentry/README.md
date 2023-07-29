To compile:

```
make -f Makefile-wasm all
```

To test:

```
python cors_server.py
```

Then navigate with your web browser to localhost:8080/bin/magpie_wasm.html

You must have the files it needs in that bin directory. Copy over all the kwg, klv2, and csv files from the data directory into this bin directory, without their paths. Then you can enter a CGP string into the Input box and click "Submit" to do a sim.