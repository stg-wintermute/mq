mq — miner query
=================

Query tool for .miner files (<<<END>>> delimited research dumps).

BUILD
-----
  make
  make install        # installs to /usr/local/bin (override with PREFIX=)


COMMANDS
--------
  meta   file|dir    Show file-level headers (topic, record count, depth, status)
  top    N query     Top N records scored by query terms
  filter expr        Filter by field expression
  grep   pattern     Regex search across title, content, keywords
  titles             List all titles grouped by file
  select fields      Print specific fields (comma-separated)
  stats              Per-file record counts by relevance tier
  jsonl              Export all records as JSONL
  count  expr        Count matching records
  dump               Dump all records
  show   file:id     Fetch one record by address
  sample N           Random N records (reservoir sampling)
  dedup  field       First unique value per field, relevance-sorted
  freq   [field]     Frequency table (default field: keywords, split on commas)
  urls               List all unique source URLs
  group  field       Records grouped under field-value headers
  slice  N[:M]       Records at positional index N, or range N..M (exclusive)

  Aliases: m=meta, f=filter, g=grep, t=titles, s=select, j=jsonl, c=count, d=dump


FLAGS
-----
  -f, --format=FORMAT    Output format: facts (default), compact, full, title, jsonl, tsv
  -n, --limit=N          Max records to output
      --preview=N        Chars of content in compact mode (default: 200)
      --no-sort          Skip relevance sort (preserve file order)
  -T, --template=FMT     Custom output: {{field}} substitution, \n and \t supported
      --exit-status      Exit 1 if no records matched (for use in scripts)
  -e  pattern            Additional grep pattern, OR semantics (repeatable)
  -w, --word             Whole-word match in grep
  Smart case:            All-lowercase grep pattern → case-insensitive automatically


EXAMPLES
--------
  # overview of a corpus
  mq meta docs/research/

  # top 10 records for a query
  mq top 10 "VLOOKUP lookup function" docs/research/

  # filter by relevance
  mq filter 'relevance == HIGH' docs/research/ -n 20

  # regex search, multiple patterns (OR)
  mq grep -e VLOOKUP -e XLOOKUP docs/research/

  # whole-word match (IF, not SUMIF)
  mq grep -w IF docs/research/ -f title

  # fetch a specific record in full
  mq show coverage-mined-data-analysts.miner:44

  # keyword frequency across corpus
  mq freq docs/research/ -n 20

  # keyword frequency for a different field
  mq freq relevance docs/research/

  # deduplicate by URL, show titles
  mq dedup url docs/research/ -f title

  # random sample
  mq sample 10 docs/research/

  # group by relevance tier, show titles
  mq group relevance docs/research/ -f title

  # records 0-19 in file order
  mq slice 0:20 docs/research/

  # all unique source URLs
  mq urls docs/research/ | sort

  # compact custom output
  mq top 5 "pivot table" docs/research/ -T '{{id}} [{{relevance}}] {{title}}'

  # TSV for piping to other tools
  mq select title,url docs/research/ -f tsv | sort

  # scripting: exit 1 if nothing found
  mq grep VLOOKUP docs/research/ --exit-status || echo "not covered"


FILE FORMAT
-----------
  MINER_DUMP
  topic: ...
  depth: N
  status: COMPLETE
  <<<END>>>

  SOURCE
  id: 1
  ts: 2026-01-01T00:00:00Z
  relevance: HIGH
  title: ...
  url: https://...
  keywords: foo, bar, baz
  content:
    ...multi-line...
  <<<END>>>
