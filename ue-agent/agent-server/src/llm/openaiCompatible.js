import process from "node:process";

export async function openAICompatibleChatCompletion({
  baseUrl,
  apiKey,
  model,
  messages,
  temperature = 0.2,
  maxTokens = 800
}) {
  const url = `${baseUrl}/v1/chat/completions`;
  const headers = {
    "Content-Type": "application/json"
  };
  if (apiKey) headers.Authorization = `Bearer ${apiKey}`;

  const body = {
    model,
    messages,
    temperature,
    max_tokens: maxTokens
  };

  const res = await fetch(url, {
    method: "POST",
    headers,
    body: JSON.stringify(body)
  });

  const text = await res.text();
  if (!res.ok) {
    throw new Error(
      `LLM error ${res.status} ${res.statusText}: ${text.slice(0, 2000)}`
    );
  }

  let json;
  try {
    json = JSON.parse(text);
  } catch (err) {
    throw new Error(
      `LLM returned non-JSON response: ${String(err)}; body: ${text.slice(0, 2000)}`
    );
  }

  const content = json?.choices?.[0]?.message?.content;
  if (typeof content !== "string") {
    throw new Error(
      `LLM response missing message.content; got: ${JSON.stringify(json).slice(0, 2000)}`
    );
  }

  return { content, raw: json };
}

