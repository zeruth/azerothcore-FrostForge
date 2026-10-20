--
-- 3.4.3 characters keep their appearance as ChrCustomizationChoice ids (decision D5): one row per
-- ChrCustomizationOption, the ChrCustomizationChoice picked for it. CMSG_CREATE_CHARACTER sends this
-- list and SMSG_ENUM_CHARACTERS_RESULT sends it back (char-create.ANALYSIS.md). The 3.3.5 skin, face,
-- hairStyle, hairColor and facialStyle columns of `characters` are no longer written by creation.
DROP TABLE IF EXISTS `character_customizations`;
CREATE TABLE `character_customizations` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `chrCustomizationOptionID` INT UNSIGNED NOT NULL COMMENT 'ChrCustomizationOption.db2',
    `chrCustomizationChoiceID` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'ChrCustomizationChoice.db2',
    PRIMARY KEY (`guid`, `chrCustomizationOptionID`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='3.4.3 character appearance choices';
