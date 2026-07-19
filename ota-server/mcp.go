package main

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type mcpCatalogInput struct {
	IncludeDrafts bool `json:"include_drafts" jsonschema:"include draft and archived boards; this MCP endpoint is administrator-only"`
}

type mcpBoardInput struct {
	Board boardCatalogEntry `json:"board" jsonschema:"complete bilingual board definition; status is always forced to draft"`
}

type mcpFeatureInput struct {
	Feature       featureCatalogEntry `json:"feature" jsonschema:"feature definition to create or update"`
	ConfirmUpdate bool                `json:"confirm_update" jsonschema:"must be true when changing an existing shared feature"`
}

type mcpAssignmentsInput struct {
	BoardID     string            `json:"board_id" jsonschema:"immutable board identifier"`
	Assignments map[string]string `json:"assignments" jsonschema:"feature key to yes, partial, or no"`
}

type mcpBoardIDInput struct {
	BoardID string `json:"board_id" jsonschema:"immutable board identifier"`
	Confirm bool   `json:"confirm" jsonschema:"must be true to publish the board publicly"`
}

type mcpImageInput struct {
	BoardID     string `json:"board_id" jsonschema:"immutable board identifier"`
	ImageBase64 string `json:"image_base64" jsonschema:"base64-encoded JPEG, PNG, or WebP image, maximum 5 MB decoded"`
}

type mcpAuditInput struct {
	Limit int `json:"limit" jsonschema:"maximum number of recent audit events, 1 to 500"`
}

type mcpAuditOutput struct {
	Events []auditEvent `json:"events"`
}

type mcpMutationOutput struct {
	ID      string `json:"id"`
	Status  string `json:"status"`
	Message string `json:"message"`
}

// mcpHandler exposes the catalog's reviewed business operations over the MCP
// Streamable HTTP transport. It is intentionally administrator-only. AI tools
// can prepare drafts freely, while public publication is a separate explicit
// call requiring confirm=true.
func (s *server) mcpHandler() http.Handler {
	ms := mcp.NewServer(&mcp.Implementation{
		Name:    "nrl-ota-catalog",
		Version: "1.0.0",
	}, nil)

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "catalog.list",
		Title:       "List OTA board catalog",
		Description: "List board types and reusable feature definitions. Use this before creating or editing a board.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpCatalogInput) (*mcp.CallToolResult, catalogResponse, error) {
		catalog, err := s.loadCatalog(input.IncludeDrafts)
		return nil, catalog, err
	})

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "audit.list",
		Title:       "List catalog audit events",
		Description: "List recent administrator and MCP changes to board and feature catalog data.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpAuditInput) (*mcp.CallToolResult, mcpAuditOutput, error) {
		events, err := s.auditEvents(input.Limit)
		return nil, mcpAuditOutput{Events: events}, err
	})

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "board.save_draft",
		Title:       "Create or update a board draft",
		Description: "Create or update bilingual board metadata. The board is always saved as a non-public draft; use board.publish separately after review.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpBoardInput) (*mcp.CallToolResult, mcpMutationOutput, error) {
		b := input.Board
		if err := validateBoard(&b, true); err != nil {
			return nil, mcpMutationOutput{}, err
		}
		b.Status = "draft"
		b.ImageURL = ""
		var existingStatus string
		if err := s.db.QueryRow(`SELECT status FROM boards WHERE id=?`, b.ID).Scan(&existingStatus); err == nil && existingStatus == "published" {
			return nil, mcpMutationOutput{}, errors.New("published boards must be moved to draft explicitly in the administrator page before AI editing")
		} else if err != nil && err != sql.ErrNoRows {
			return nil, mcpMutationOutput{}, err
		}
		zh, _ := json.Marshal(b.HighlightsZH)
		en, _ := json.Marshal(b.HighlightsEN)
		now := time.Now().Unix()
		_, err := s.db.Exec(`INSERT INTO boards(id,name_zh,name_en,tagline_zh,tagline_en,description_zh,description_en,chip_label,web_flash_chip_family,image_url,display_order,status,highlights_zh_json,highlights_en_json,created_at,updated_at)
			VALUES(?,?,?,?,?,?,?,?,?,?,?,'draft',?,?,?,?)
			ON CONFLICT(id) DO UPDATE SET name_zh=excluded.name_zh,name_en=excluded.name_en,tagline_zh=excluded.tagline_zh,tagline_en=excluded.tagline_en,description_zh=excluded.description_zh,description_en=excluded.description_en,chip_label=excluded.chip_label,web_flash_chip_family=excluded.web_flash_chip_family,display_order=excluded.display_order,status='draft',highlights_zh_json=excluded.highlights_zh_json,highlights_en_json=excluded.highlights_en_json,updated_at=excluded.updated_at`,
			b.ID, b.NameZH, b.NameEN, b.TaglineZH, b.TaglineEN, b.DescriptionZH, b.DescriptionEN, b.ChipLabel, b.WebFlashChipFamily, b.ImageURL, b.DisplayOrder, string(zh), string(en), now, now)
		if err != nil {
			return nil, mcpMutationOutput{}, err
		}
		s.recordAudit("mcp", "board.save_draft", b.ID, map[string]any{"status": "draft"})
		return nil, mcpMutationOutput{ID: b.ID, Status: "draft", Message: "board draft saved"}, nil
	})

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "feature.save",
		Title:       "Create or update a feature",
		Description: "Create a reusable bilingual feature definition or update an existing definition by immutable key.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpFeatureInput) (*mcp.CallToolResult, mcpMutationOutput, error) {
		f := input.Feature
		if err := validateFeature(&f, true); err != nil {
			return nil, mcpMutationOutput{}, err
		}
		var one int
		if err := s.db.QueryRow(`SELECT 1 FROM features WHERE key=?`, f.Key).Scan(&one); err == nil && !input.ConfirmUpdate {
			return nil, mcpMutationOutput{}, errors.New("confirm_update must be true when changing an existing shared feature")
		} else if err != nil && err != sql.ErrNoRows {
			return nil, mcpMutationOutput{}, err
		}
		_, err := s.db.Exec(`INSERT INTO features(key,label_zh,label_en,description_zh,description_en,group_name,display_order,active) VALUES(?,?,?,?,?,?,?,?)
			ON CONFLICT(key) DO UPDATE SET label_zh=excluded.label_zh,label_en=excluded.label_en,description_zh=excluded.description_zh,description_en=excluded.description_en,group_name=excluded.group_name,display_order=excluded.display_order,active=excluded.active`,
			f.Key, f.LabelZH, f.LabelEN, f.DescriptionZH, f.DescriptionEN, f.GroupName, f.DisplayOrder, boolInt(f.Active))
		if err != nil {
			return nil, mcpMutationOutput{}, err
		}
		s.recordAudit("mcp", "feature.save", f.Key, map[string]any{"active": f.Active})
		return nil, mcpMutationOutput{ID: f.Key, Status: "saved", Message: "feature saved"}, nil
	})

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "board.set_features",
		Title:       "Set a board feature matrix",
		Description: "Replace all feature assignments for a board. Values must be yes, partial, or no; call catalog.list first to get valid feature keys.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpAssignmentsInput) (*mcp.CallToolResult, mcpMutationOutput, error) {
		if !s.boardExists(input.BoardID) {
			return nil, mcpMutationOutput{}, errors.New("board not found")
		}
		if s.boardPublished(input.BoardID) {
			return nil, mcpMutationOutput{}, errors.New("published boards must be moved to draft explicitly in the administrator page before AI editing")
		}
		tx, err := s.db.Begin()
		if err != nil {
			return nil, mcpMutationOutput{}, err
		}
		defer tx.Rollback()
		if _, err = tx.Exec(`DELETE FROM board_features WHERE board_id=?`, input.BoardID); err != nil {
			return nil, mcpMutationOutput{}, err
		}
		for key, state := range input.Assignments {
			if state != "yes" && state != "partial" && state != "no" {
				return nil, mcpMutationOutput{}, fmt.Errorf("invalid feature state for %s", key)
			}
			var one int
			if err = tx.QueryRow(`SELECT 1 FROM features WHERE key=?`, key).Scan(&one); err != nil {
				return nil, mcpMutationOutput{}, fmt.Errorf("unknown feature: %s", key)
			}
			if _, err = tx.Exec(`INSERT INTO board_features(board_id,feature_key,state) VALUES(?,?,?)`, input.BoardID, key, state); err != nil {
				return nil, mcpMutationOutput{}, err
			}
		}
		if err = tx.Commit(); err != nil {
			return nil, mcpMutationOutput{}, err
		}
		s.recordAudit("mcp", "board.set_features", input.BoardID, map[string]any{"features": len(input.Assignments)})
		return nil, mcpMutationOutput{ID: input.BoardID, Status: "draft", Message: "feature assignments saved"}, nil
	})

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "board.upload_image",
		Title:       "Upload a board image",
		Description: "Upload a base64 JPEG, PNG, or WebP image to an existing board draft. Decoded size is limited to 5 MB and SVG is rejected.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpImageInput) (*mcp.CallToolResult, mcpMutationOutput, error) {
		if s.boardPublished(input.BoardID) {
			return nil, mcpMutationOutput{}, errors.New("published boards must be moved to draft explicitly in the administrator page before AI editing")
		}
		encoded := input.ImageBase64
		if before, after, ok := strings.Cut(encoded, ","); ok && strings.HasPrefix(before, "data:image/") {
			encoded = after
		}
		data, err := base64.StdEncoding.DecodeString(encoded)
		if err != nil {
			return nil, mcpMutationOutput{}, errors.New("image_base64 is invalid")
		}
		imageURL, err := s.saveBoardImage(input.BoardID, data)
		if err != nil {
			return nil, mcpMutationOutput{}, err
		}
		s.recordAudit("mcp", "board.upload_image", input.BoardID, map[string]any{"image_url": imageURL})
		return nil, mcpMutationOutput{ID: input.BoardID, Status: "draft", Message: imageURL}, nil
	})

	mcp.AddTool(ms, &mcp.Tool{
		Name:        "board.publish",
		Title:       "Publish a board publicly",
		Description: "Publish a reviewed board. Requires confirm=true, a board image, and at least one configured feature. This makes it visible on the OTA home page and available for firmware uploads.",
	}, func(_ context.Context, _ *mcp.CallToolRequest, input mcpBoardIDInput) (*mcp.CallToolResult, mcpMutationOutput, error) {
		if !input.Confirm {
			return nil, mcpMutationOutput{}, errors.New("confirm must be true")
		}
		var nameZH, nameEN string
		if err := s.db.QueryRow(`SELECT name_zh,name_en FROM boards WHERE id=?`, input.BoardID).Scan(&nameZH, &nameEN); err != nil {
			return nil, mcpMutationOutput{}, errors.New("board not found")
		}
		if err := s.ensureBoardPublishable(input.BoardID, nameZH, nameEN); err != nil {
			return nil, mcpMutationOutput{}, err
		}
		if _, err := s.db.Exec(`UPDATE boards SET status='published',updated_at=? WHERE id=?`, time.Now().Unix(), input.BoardID); err != nil {
			return nil, mcpMutationOutput{}, err
		}
		s.recordAudit("mcp", "board.publish", input.BoardID, map[string]any{"status": "published"})
		return nil, mcpMutationOutput{ID: input.BoardID, Status: "published", Message: "board published"}, nil
	})

	handler := mcp.NewStreamableHTTPHandler(func(*http.Request) *mcp.Server { return ms }, &mcp.StreamableHTTPOptions{
		Stateless:                  true,
		JSONResponse:               true,
		DisableLocalhostProtection: true, // nginx terminates HTTPS and forwards from 127.0.0.1 with the public Host
	})
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			r.Body = http.MaxBytesReader(w, r.Body, 8<<20)
		}
		if !s.isAdminRequest(r) {
			w.Header().Set("WWW-Authenticate", `Bearer realm="NRL OTA MCP"`)
			http.Error(w, "admin authorization required", http.StatusUnauthorized)
			return
		}
		handler.ServeHTTP(w, r)
	})
}
