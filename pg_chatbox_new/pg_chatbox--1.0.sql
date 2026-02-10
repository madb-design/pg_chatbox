CREATE FUNCTION chatbox_query(text) RETURNS text
AS 'MODULE_PATHNAME','chatbox_query'
LANGUAGE C STRICT;

CREATE FUNCTION chatbox_explain(text) RETURNS text
AS 'MODULE_PATHNAME','chatbox_explain'
LANGUAGE C STRICT;

CREATE FUNCTION chatbox_explain_plan(text) RETURNS text
AS 'MODULE_PATHNAME','chatbox_explain_plan'
LANGUAGE C STRICT;
