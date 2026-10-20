SELECT
  CASE
    WHEN COALESCE(topic,'') = '' THEN '<EMPTY>'
    WHEN INSTR(topic,'$tenants/') = 1 THEN '$tenants/.../guo_feeder/<X>/<dir>'
    WHEN INSTR(topic,'guo_feeder/') = 1 THEN 'guo_feeder/<X>/<dir>'
    ELSE 'OTHER:' || substr(topic,1,40)
  END AS shape,
  COUNT(*) AS n
FROM mqtt_messages
GROUP BY shape
ORDER BY n DESC
LIMIT 20;
