// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#import "CommitListController.h"

#import "CommitCell.h"
#import "CommitListPresenter.h"

@interface CommitListController ()

@property CommitListPresenter* presenter;

@end

@implementation CommitListController

- (void)viewDidLoad {
  [super viewDidLoad];
  self.presenter = [[CommitListPresenter alloc] init:self.tableView];

  // Setup regular refresh.
  CADisplayLink* consoleLink =
      [CADisplayLink displayLinkWithTarget:self.presenter
                                  selector:@selector(refresh)];
  [consoleLink setFrameInterval:60];
  [consoleLink addToRunLoop:[NSRunLoop currentRunLoop]
                    forMode:NSDefaultRunLoopMode];

}

- (void)didReceiveMemoryWarning {
  [super didReceiveMemoryWarning];
  // Dispose of any resources that can be recreated.
}

#pragma mark - Table view data source

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
  assert(section == 0);
  return self.presenter.commitCount;
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
  CommitCell* cell =
    (CommitCell*)[tableView
        dequeueReusableCellWithIdentifier:CommitCellId
                             forIndexPath:indexPath];
  CommitNode* commit = [self.presenter commitAtIndex:indexPath.row];
  cell.authorLabel.text = commit.author;
  cell.messageLabel.text = commit.message;
  return cell;
}

@end
