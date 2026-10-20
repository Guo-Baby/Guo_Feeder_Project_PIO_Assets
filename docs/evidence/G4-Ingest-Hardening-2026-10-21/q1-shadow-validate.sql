SELECT
  COUNT(*) AS n_total,
  SUM(CASE WHEN COALESCE(topic,'') = '' THEN 1 ELSE 0 END) AS n_topic_empty,
  SUM(CASE WHEN COALESCE(topic,'') <> '' AND INSTR(topic,'guo_feeder') = 0 THEN 1 ELSE 0 END) AS n_topic_no_guofeeder,
  SUM(CASE WHEN COALESCE(direction,'') NOT IN ('up','down','log') THEN 1 ELSE 0 END) AS n_dir_not_whitelist,
  SUM(CASE WHEN COALESCE(raw_payload,'') = '' THEN 1 ELSE 0 END) AS n_payload_empty,
  SUM(CASE WHEN COALESCE(msg_id,'') = '' THEN 1 ELSE 0 END) AS n_msgid_empty,
  SUM(CASE WHEN COALESCE(topic,'') <> '' AND INSTR(topic,'guo_feeder') > 0
            AND COALESCE(direction,'') IN ('up','down','log')
            AND COALESCE(raw_payload,'') <> ''
           THEN 0 ELSE 1 END) AS n_reject_strict
FROM mqtt_messages;
