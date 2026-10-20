SELECT
  COUNT(*) AS n_total,
  SUM(CASE WHEN topic LIKE '%/up'   THEN 1 ELSE 0 END) AS n_suffix_up,
  SUM(CASE WHEN topic LIKE '%/down' THEN 1 ELSE 0 END) AS n_suffix_down,
  SUM(CASE WHEN topic LIKE '%/log'  THEN 1 ELSE 0 END) AS n_suffix_log,
  SUM(CASE WHEN topic LIKE '%/up' OR topic LIKE '%/down' OR topic LIKE '%/log'
           THEN 0 ELSE 1 END) AS n_suffix_other,
  SUM(CASE WHEN (topic LIKE '%/up' OR topic LIKE '%/down' OR topic LIKE '%/log')
            AND INSTR(topic,'guo_feeder') > 0
            AND COALESCE(raw_payload,'') <> ''
           THEN 0 ELSE 1 END) AS n_reject_s2
FROM mqtt_messages;
