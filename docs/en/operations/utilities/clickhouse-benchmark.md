---
description: 'Documentation for clickhouse-benchmark '
sidebar_label: 'clickhouse-benchmark'
sidebar_position: 61
slug: /operations/utilities/clickhouse-benchmark
title: 'clickhouse-benchmark'
---

# clickhouse-benchmark 

Connects to a ClickHouse server and repeatedly sends specified queries.

**Syntax**

```bash
$ clickhouse-benchmark --query ["single query"] [keys]
```

or

```bash
$ echo "single query" | clickhouse-benchmark [keys]
```

or

```bash
$ clickhouse-benchmark [keys] <<< "single query"
```

If you want to send a set of queries, create a text file and place each query on the individual string in this file. For example:

```sql
SELECT * FROM system.numbers LIMIT 10000000;
SELECT 1;
```

Then pass this file to a standard input of `clickhouse-benchmark`:

```bash
clickhouse-benchmark [keys] < queries_file;
```

## Keys {#clickhouse-benchmark-keys}

- `--query=QUERY` — Query to execute. If this parameter is not passed, `clickhouse-benchmark` will read queries from standard input.
- `-c N`, `--concurrency=N` — Number of queries that `clickhouse-benchmark` sends simultaneously. Default value: 1.
- `-d N`, `--delay=N` — Interval in seconds between intermediate reports (to disable reports set 0). Default value: 1.
- `-h HOST`, `--host=HOST` — Server host. Default value: `localhost`. For the [comparison mode](#clickhouse-benchmark-comparison-mode) you can use multiple `-h` keys.
- `-i N`, `--iterations=N` — Total number of queries. Default value: 0 (repeat forever).
- `-r`, `--randomize` — Random order of queries execution if there is more than one input query.
- `-s`, `--secure` — Using `TLS` connection.
- `-t N`, `--timelimit=N` — Time limit in seconds. `clickhouse-benchmark` stops sending queries when the specified time limit is reached. Default value: 0 (time limit disabled).
- `--port=N` — Server port. Default value: 9000. For the [comparison mode](#clickhouse-benchmark-comparison-mode) you can use multiple `--port` keys.
- `--confidence=N` — Level of confidence for T-test. Possible values: 0 (80%), 1 (90%), 2 (95%), 3 (98%), 4 (99%), 5 (99.5%). Default value: 5. In the [comparison mode](#clickhouse-benchmark-comparison-mode) `clickhouse-benchmark` performs the [Independent two-sample Student's t-test](https://en.wikipedia.org/wiki/Student%27s_t-test#Independent_two-sample_t-test) to determine whether the two distributions aren't different with the selected level of confidence.
- `--cumulative` — Printing cumulative data instead of data per interval.
- `--database=DATABASE_NAME` — ClickHouse database name. Default value: `default`.
- `--user=USERNAME` — ClickHouse user name. Default value: `default`.
- `--password=PSWD` — ClickHouse user password. Default value: empty string.
- `--stacktrace` — Stack traces output. When the key is set, `clickhouse-bencmark` outputs stack traces of exceptions.
- `--stage=WORD` — Query processing stage at server. ClickHouse stops query processing and returns an answer to `clickhouse-benchmark` at the specified stage. Possible values: `complete`, `fetch_columns`, `with_mergeable_state`. Default value: `complete`.
- `--reconnect=N` - Control reconnection behaviour. Possible values 0 (never reconnect), 1 (reconnect for every query), or N (reconnect after every N queries). Default value: 0.
- `--help` — Shows the help message.

If you want to apply some [settings](/operations/settings/overview) for queries, pass them as a key `--<session setting name>= SETTING_VALUE`. For example, `--max_memory_usage=1048576`.

## Output {#clickhouse-benchmark-output}

By default, `clickhouse-benchmark` reports for each `--delay` interval.

Example of the report:

```text
Queries executed: 10.

localhost:9000, queries 10, QPS: 6.772, RPS: 67904487.440, MiB/s: 518.070, result RPS: 67721584.984, result MiB/s: 516.675.

0.000%      0.145 sec.
10.000%     0.146 sec.
20.000%     0.146 sec.
30.000%     0.146 sec.
40.000%     0.147 sec.
50.000%     0.148 sec.
60.000%     0.148 sec.
70.000%     0.148 sec.
80.000%     0.149 sec.
90.000%     0.150 sec.
95.000%     0.150 sec.
99.000%     0.150 sec.
99.900%     0.150 sec.
99.990%     0.150 sec.
```

In the report you can find:

- Number of queries in the `Queries executed:` field.

- Status string containing (in order):

  - Endpoint of ClickHouse server.
  - Number of processed queries.
  - QPS: How many queries the server performed per second during a period specified in the `--delay` argument.
  - RPS: How many rows the server reads per second during a period specified in the `--delay` argument.
  - MiB/s: How many mebibytes the server reads per second during a period specified in the `--delay` argument.
  - result RPS: How many rows placed by the server to the result of a query per second during a period specified in the `--delay` argument.
  - result MiB/s. How many mebibytes placed by the server to the result of a query per second during a period specified in the `--delay` argument.

- Percentiles of queries execution time.

## Comparison Mode {#clickhouse-benchmark-comparison-mode}

`clickhouse-benchmark` can compare performances for two running ClickHouse servers.

To use the comparison mode, specify endpoints of both servers by two pairs of `--host`, `--port` keys. Keys matched together by position in arguments list, the first `--host` is matched with the first `--port` and so on. `clickhouse-benchmark` establishes connections to both servers, then sends queries. Each query addressed to a randomly selected server. The results are shown in a table.

## Example {#clickhouse-benchmark-example}

```bash
$ echo "SELECT * FROM system.numbers LIMIT 10000000 OFFSET 10000000" | clickhouse-benchmark --host=localhost --port=9001 --host=localhost --port=9000 -i 10
```

```text
Loaded 1 queries.

Queries executed: 5.

localhost:9001, queries 2, QPS: 3.764, RPS: 75446929.370, MiB/s: 575.614, result RPS: 37639659.982, result MiB/s: 287.168.
localhost:9000, queries 3, QPS: 3.815, RPS: 76466659.385, MiB/s: 583.394, result RPS: 38148392.297, result MiB/s: 291.049.

0.000%          0.258 sec.      0.250 sec.
10.000%         0.258 sec.      0.250 sec.
20.000%         0.258 sec.      0.250 sec.
30.000%         0.258 sec.      0.267 sec.
40.000%         0.258 sec.      0.267 sec.
50.000%         0.273 sec.      0.267 sec.
60.000%         0.273 sec.      0.267 sec.
70.000%         0.273 sec.      0.267 sec.
80.000%         0.273 sec.      0.269 sec.
90.000%         0.273 sec.      0.269 sec.
95.000%         0.273 sec.      0.269 sec.
99.000%         0.273 sec.      0.269 sec.
99.900%         0.273 sec.      0.269 sec.
99.990%         0.273 sec.      0.269 sec.

No difference proven at 99.5% confidence
```
