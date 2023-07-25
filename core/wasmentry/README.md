To compile:

```
make -f Makefile-wasm all
```

To test in Node:

```js
const module = require('./magpie_wasm');
cgp = module.stringToNewUTF8("C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 lex NWL20;")
// Note:  cgp is a number now; it's a pointer to an array:
module.ccall('sim_position', null, ['number'], [cgp]);
module._free(cgp);  // clean up after yourself.
```