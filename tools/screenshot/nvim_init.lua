-- nvim for the clod screenshot (run with --clean, after the file loads): the default look, but on
-- the terminal's own background so the pane stays see-through like the rest of clodw.
vim.o.termguicolors = true
vim.o.number = true
vim.o.wrap = false
vim.o.laststatus = 2
vim.o.swapfile = false
for _, group in ipairs({ 'Normal', 'NormalNC', 'SignColumn', 'EndOfBuffer', 'LineNr', 'CursorLineNr', 'FoldColumn' }) do
    vim.cmd('highlight ' .. group .. ' guibg=NONE ctermbg=NONE')
end

-- show the edge collapse loop, cursor inside it
vim.cmd('normal! 33Gzt')
vim.cmd('normal! 7j^')
