SCENE  := scenes/keyboard.sdf
CONFIG := target/configs/viewer_fast.toml

render viewer export test clean:
	@ target/workflow.py $@ $(SCENE) $(CONFIG)
