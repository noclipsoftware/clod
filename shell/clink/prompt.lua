-- The cmd prompt (cwd), with the input on its own line after a lambda.
local lambda_prompt = clink.promptfilter(150)
function lambda_prompt:filter(prompt)
    return prompt:gsub(">%s*$", "").."\nλ "
end
