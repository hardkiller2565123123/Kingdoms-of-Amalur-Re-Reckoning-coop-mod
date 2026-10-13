#pragma once

// Executed by the game's own HavokScript VM after the original main_menu
// module finishes loading. Native button_tree owns layout, focus and input.
inline constexpr char kMultiplayerMenuScript[] = R"AmalurLua(
local installed, install_error = pcall(function()
local menu = _G.main_menu
assert(menu, "main_menu module unavailable")
if menu and not menu.amalur_coop_installed then
    local original_info = menu.get_button_tree_info
    local original_update = menu.on_update_event
    local multiplayer_id = 0x414301
    local host_id = 0x414302
    local join_id = 0x414303
    local status_id = 0x414304
    local status = "Choose Host or Join, then load your save."
    local ticks = 0
    local function choose_save()
        -- Select the existing Load Game node through native navigation. This
        -- preserves save-device checks, lazy window loading and Back behavior.
        local tree = menu.m_button_tree
        local load = tree and tree:find_node(735424)
        assert(load and not load.disabled, "Load Game is unavailable")
        tree:launch_into(657596)
        tree:set_highlight(tree.m_lookup_table[735424].index)
        tree:go_down_a_level()
    end
    local function refresh_status(text)
        status = text or status
        if menu.m_button_tree then
            menu.m_button_tree:change_button_title(status_id, status)
        end
    end
    local function host()
        refresh_status(print("__AmalurCoop:host"))
        choose_save()
    end
    local function join()
        refresh_status(print("__AmalurCoop:join"))
        choose_save()
    end
    -- Module children go through build_tree, which supplies parent/root links
    -- and controller navigation metadata for every Host/Join leaf.
    -- loadstring inherits the main_menu module environment in this VM.
    -- button_tree resolves module names in _G, not in main_menu's fields.
    _G.amalur_coop_menu = { button_tree_callback = function() end }
    _G.amalur_coop_menu.get_button_tree_info = function()
        return {
            id_string = multiplayer_id,
            title = "Multiplayer",
            description = "Play together. Start a session, then load your save.",
            extra_wins = {},
            children = {
                { id_string = host_id, title = "Host", override = host,
                  description = "Host a co-op session on the configured port.", children = {} },
                { id_string = join_id, title = "Join", override = join,
                  description = "Join the configured address, or discover a LAN host automatically.", children = {} },
                { id_string = status_id, title = status, disabled = true, children = {} }
            }
        }
    end
    menu.get_button_tree_info = function(...)
        local root = original_info(...)
        root.children = root.children or {}
        local at = #root.children + 1
        for index, child in ipairs(root.children) do
            if child.module == "options_win" then at = index; break end
        end
        table.insert(root.children, at, { module = "amalur_coop_menu" })
        return root
    end
    menu.on_update_event = function(...)
        if original_update then original_update(...) end
        ticks = ticks + 1
        if ticks >= 60 then
            ticks = 0
            local next_status = print("__AmalurCoop:status")
            if next_status and next_status ~= status then refresh_status(next_status) end
        end
    end
    menu.amalur_coop_installed = true
    print("__AmalurCoop:ready")
end
end)
if not installed then print("__AmalurCoop:error:" .. string.sub(tostring(install_error), 1, 450)) end
)AmalurLua";
