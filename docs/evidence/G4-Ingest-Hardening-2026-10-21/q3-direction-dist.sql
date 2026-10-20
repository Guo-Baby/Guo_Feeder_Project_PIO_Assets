SELECT
  COALESCE(direction,'<NULL>') AS dir,
  COUNT(*) AS n,
  MIN(id) AS min_id,
  MAX(id) AS max_id
FROM mqtt_messages
GROUP BY dir
ORDER BY n DESC;
