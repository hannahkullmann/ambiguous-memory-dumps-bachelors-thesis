# Ambiguous Memory Dumps — Bachelor's Thesis

This repository contains a modified Linux kernel and analysis results for a bachelor's thesis on ambiguous memory dumps: a single physical memory image holds both host and guest memory at once

**Note:** Large used files (memory dumps, vmlinux, ) are intentionally not in git as they exceed the upload limit; they can be requested via mail to [hannah.kullmann@fau.de](mailto:hannah.kullmann@fau.de)

### Repository layout

```
ambiguous-memory-dumps-bachelors-thesis/
├── README.md
├── adjusted_kernel_linux-6.18.7/     # modified kernel 
└── analysis/
    ├── host_dump/                    # host context (modified kernel with ingested guest OS)
    |    └── caved_out_region         # guest context reverse engineered from host context  
    ├── ingested_dump/                # guest context (ingested guest OS)
    └── results/volatility3/          # archived plugin output

```


| Subfolders of `results/volatility3/` | content                                   |
| ------------------------------------ | ----------------------------------------- |
| `vboxmanage_ambig_dump/`             |                                           |
| ├── `host_symbols/`                  | analysis using host-related symbol table  |
| └── `guest_symbols/`                 | analysis using guest-related symbol table |
| `avml_ingested_dump/`                | analysis of the ingested dump             |
| `carved_out_region/`                 | analysis fo the carved out region         |


