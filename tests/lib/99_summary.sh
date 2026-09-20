# ============================================================
# Final summary
# ============================================================
TOTAL=$((PASS + FAIL))
# Calculate elapsed time
TOTAL_MINUTES=$((SECONDS / 60))
TOTAL_SECONDS=$((SECONDS % 60))
log ""
log "${BOLD}============================================================${NC}"
log "${BOLD}⏱️  TOTAL EXECUTION TIME: ${TOTAL_MINUTES}m ${TOTAL_SECONDS}s${NC}"
log "${BOLD}============================================================${NC}"
if [ "$FAIL" -eq 0 ]; then
log "${GREEN}${BOLD}RESULT: $PASS/$TOTAL tests passed${NC}"
log "${GREEN}Log saved at: $LOG_FILE${NC}"
#rm -rf "$WORK"
exit 0
else
log "${RED}${BOLD}RESULT: $PASS/$TOTAL tests passed, $FAIL failed${NC}"
log "${YELLOW}Log saved at: $LOG_FILE${NC}"
log ""
log "Failed tests:"
for t in "${FAILED_TESTS[@]}"; do
log "  ${RED}- $t${NC}"
done
#rm -rf "$WORK"
exit 1
fi

